#include "RtcClock.h"
#include "RtcConfig.h"

#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "RTC";
static const int TIMEOUT_MS = 100;
typedef struct { int year, month, day, hour, minute, second; } DateTime;
typedef struct {
    DateTime time;
    TickType_t sampledAt;
    bool valid;
} Sample;
static QueueHandle_t samples = NULL;
static i2c_master_dev_handle_t device = NULL;

static bool valid(const DateTime *t) {
    if (t->year < 2000 || t->year > 2099 || t->month < 1 || t->month > 12 ||
        t->day < 1 || t->hour < 0 || t->hour > 23 || t->minute < 0 ||
        t->minute > 59 || t->second < 0 || t->second > 59) return false;
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return t->day <= days[t->month - 1] + (t->month == 2 && t->year % 4 == 0);
}

static int fromBcd(uint8_t value) {
    if ((value & 15) > 9 || (value >> 4) > 9) return -1;
    return (value >> 4) * 10 + (value & 15);
}

static uint8_t toBcd(int value) { return (uint8_t)((value / 10) * 16 + value % 10); }

static esp_err_t readRegisters(uint8_t address, uint8_t *data, size_t size) {
    return i2c_master_transmit_receive(device, &address, 1, data, size, TIMEOUT_MS);
}

static esp_err_t readTime(DateTime *t, bool *needsSetting) {
    uint8_t config[2] = {0};
    esp_err_t result = readRegisters(0x0E, config, sizeof(config));
    if (result != ESP_OK) return result;
    // Preserva a hora existente e garante contagem tambem pela bateria.
    if (config[0] & 0x80) {
        const uint8_t enable[] = {0x0E, (uint8_t)(config[0] & ~0x80)};
        result = i2c_master_transmit(device, enable, sizeof(enable), TIMEOUT_MS);
        if (result != ESP_OK) return result;
    }
    uint8_t data[7];
    // Leitura em bloco usa o snapshot dos registradores do DS3232M.
    result = readRegisters(0x00, data, sizeof(data));
    if (result != ESP_OK) return result;
    *t = (DateTime){2000 + fromBcd(data[6]), fromBcd(data[5] & 0x1F),
         fromBcd(data[4] & 0x3F), fromBcd(data[2] & 0x3F),
         fromBcd(data[1] & 0x7F), fromBcd(data[0] & 0x7F)};
    if (data[2] & 0x40) {
        const int hour12 = fromBcd(data[2] & 0x1F);
        t->hour = hour12 >= 1 && hour12 <= 12
            ? hour12 % 12 + ((data[2] & 0x20) ? 12 : 0) : -1;
    }
    // Este monitor usa 2000..2099. Nao interpreta seculo 2100 como 2000.
    if (data[5] & 0x80) return ESP_ERR_NOT_SUPPORTED;
    *needsSetting = (config[1] & 0x80) || !valid(t);
    return ESP_OK;
}

static esp_err_t setBuildTime(void) {
    char month[4] = {0};
    DateTime t = {0};
    if (sscanf(__DATE__, "%3s %d %d", month, &t.day, &t.year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &t.hour, &t.minute, &t.second) != 3)
        return ESP_ERR_INVALID_ARG;
    const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *found = strstr(months, month);
    if (!found) return ESP_ERR_INVALID_ARG;
    t.month = (int)(found - months) / 3 + 1;
    if (!valid(&t)) return ESP_ERR_INVALID_ARG;
    // Dia da semana: domingo=1, conforme convencao adotada aqui.
    static const int offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    const int y = t.year - (t.month < 3);
    const int weekday = (y + y/4 - y/100 + y/400 + offsets[t.month-1] + t.day) % 7 + 1;
    const uint8_t data[] = {0, toBcd(t.second), toBcd(t.minute), toBcd(t.hour),
        (uint8_t)(weekday), toBcd(t.day), toBcd(t.month), toBcd(t.year - 2000)};
    esp_err_t result = i2c_master_transmit(device, data, sizeof(data), TIMEOUT_MS);
    if (result != ESP_OK) return result;
    uint8_t control = 0;
    result = readRegisters(0x0E, &control, 1);
    if (result != ESP_OK) return result;
    // EOSC=0 permite continuar contando quando alimentado pela bateria.
    const uint8_t enable[] = {0x0E, (uint8_t)(control & ~0x80)};
    result = i2c_master_transmit(device, enable, sizeof(enable), TIMEOUT_MS);
    if (result != ESP_OK) return result;
    uint8_t status = 0;
    result = readRegisters(0x0F, &status, 1);
    if (result != ESP_OK) return result;
    const uint8_t clearOsf[] = {0x0F, (uint8_t)(status & ~0x80)};
    result = i2c_master_transmit(device, clearOsf, sizeof(clearOsf), TIMEOUT_MS);
    if (result == ESP_OK) {
        ESP_LOGW(TAG, "RTC sem hora valida: ajustado para COMPILACAO %s %s. "
                 "Horario aproximado; inclui atraso ate a gravacao. Fuso do computador.",
                 __DATE__, __TIME__);
    }
    return result;
}

static void clockTask(void *context) {
    (void)context;
    esp_err_t previous = ESP_OK;
    for (;;) {
        Sample sample = {0};
        bool needsSetting = false;
        esp_err_t result = readTime(&sample.time, &needsSetting);
        if (result == ESP_OK && needsSetting) {
            result = RTC_INITIALIZE_FROM_BUILD_TIME
                ? setBuildTime() : ESP_ERR_INVALID_STATE;
            if (result == ESP_OK) result = readTime(&sample.time, &needsSetting);
            if (result == ESP_OK && needsSetting) result = ESP_ERR_INVALID_STATE;
        }
        sample.valid = result == ESP_OK;
        sample.sampledAt = xTaskGetTickCount();
        xQueueOverwrite(samples, &sample);
        if (result != previous) {
            if (result == ESP_OK) ESP_LOGI(TAG, "Leitura do RTC restabelecida");
            else ESP_LOGW(TAG, "Horario indisponivel: %s; nova tentativa em 1 s",
                          esp_err_to_name(result));
            previous = result;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void rtc_clock_start(void) {
    samples = xQueueCreate(1, sizeof(Sample));
    if (!samples) {
        ESP_LOGE(TAG, "Falha ao criar fila do RTC");
        return;
    }
    i2c_master_bus_config_t config = {0};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = RTC_SDA;
    config.scl_io_num = RTC_SCL;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t result = i2c_new_master_bus(&config, &bus);
    if (result == ESP_OK) {
        i2c_device_config_t devConfig = {0};
        devConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        devConfig.device_address = 0x68;
        devConfig.scl_speed_hz = 100000;
        result = i2c_master_bus_add_device(bus, &devConfig, &device);
    }
    if (result == ESP_OK &&
        xTaskCreate(clockTask, "rtc_ds3232m", 3072, NULL, 2, NULL) != pdPASS)
        result = ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar RTC: %s", esp_err_to_name(result));
        if (device) i2c_master_bus_rm_device(device);
        if (bus) i2c_del_master_bus(bus);
        device = NULL;
        vQueueDelete(samples);
        samples = NULL;
        return;
    }
    ESP_LOGI(TAG, "DS3232M: SDA=GPIO%d SCL=GPIO%d endereco=0x68",
             (int)(RTC_SDA), (int)(RTC_SCL));
}

bool rtc_clock_timestamp(char *buffer, size_t size) {
    if (!buffer || size == 0) return false;
    Sample sample = {0};
    if (!samples || xQueuePeek(samples, &sample, 0) != pdTRUE || !sample.valid ||
        xTaskGetTickCount() - sample.sampledAt > pdMS_TO_TICKS(2500)) {
        snprintf(buffer, size, "RTC INDISPONIVEL");
        return false;
    }
    const DateTime *t = &sample.time;
    const int written = snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d",
        t->year, t->month, t->day, t->hour, t->minute, t->second);
    return written >= 0 && (size_t)(written) < size;
}
