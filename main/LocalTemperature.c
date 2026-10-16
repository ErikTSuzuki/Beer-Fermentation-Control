#include "LocalTemperature.h"
#include "RtcClock.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "ds18b20.h"

// DS18B20 alimentado por 3,3 V: DQ no GPIO4 e pull-up externo de 4,7 kohm.
static const int DS18B20_GPIO = 4;
static const uint32_t LOCAL_SAMPLE_PERIOD_MS = 2000;
static const char *LOCAL_TAG = "DS18B20_LOCAL";

// Caixa postal com a amostra mais recente; acesso seguro entre as tarefas.
static QueueHandle_t localTemperatureQueue = NULL;

static esp_err_t findLocalSensor(onewire_bus_handle_t bus,
                                 ds18b20_device_handle_t *sensor) {
    onewire_device_iter_handle_t iter = NULL;
    esp_err_t result = onewire_new_device_iter(bus, &iter);
    if (result != ESP_OK) {
        return result;
    }

    onewire_device_t device = {0};
    while ((result = onewire_device_iter_get_next(iter, &device)) == ESP_OK) {
        ds18b20_config_t config;
        // Esta versao da biblioteca usa uma estrutura de configuracao vazia.
        memset(&config, 0, sizeof(config));
        result = ds18b20_new_device_from_enumeration(&device, &config, sensor);
        if (result != ESP_ERR_NOT_SUPPORTED) {
            break;
        }
    }
    onewire_del_device_iter(iter);
    return result;
}

static void localTemperatureTask(void *context) {
    (void)context;
    onewire_bus_handle_t bus = NULL;
    onewire_bus_config_t busConfig = {0};
    busConfig.bus_gpio_num = DS18B20_GPIO;
    onewire_bus_rmt_config_t rmtConfig = {0};
    rmtConfig.max_rx_bytes = 10;

    esp_err_t result = onewire_new_bus_rmt(&busConfig, &rmtConfig, &bus);
    if (result != ESP_OK) {
        ESP_LOGE(LOCAL_TAG, "Falha ao iniciar GPIO%d: %s",
                 DS18B20_GPIO, esp_err_to_name(result));
        vTaskDelete(NULL);
        return;
    }

    ds18b20_device_handle_t sensor = NULL;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        LocalTemperature sample = {0};
        result = ESP_OK;
        if (!sensor) {
            result = findLocalSensor(bus, &sensor);
            if (result == ESP_OK) {
                result = ds18b20_set_resolution(sensor, DS18B20_RESOLUTION_12B);
            }
        }
        if (result == ESP_OK) {
            // A biblioteca espera a conversao nesta tarefa, sem bloquear o ACK.
            result = ds18b20_trigger_temperature_conversion(sensor);
        }
        if (result == ESP_OK) {
            result = ds18b20_get_temperature(sensor, &sample.temperatureC);
        }
        if (result == ESP_OK &&
            (!isfinite(sample.temperatureC) ||
             sample.temperatureC < -55.0f || sample.temperatureC > 125.0f ||
             sample.temperatureC == 85.0f)) {
            // 85 C e o valor de inicializacao; inadequado para esta comparacao.
            result = ESP_ERR_INVALID_RESPONSE;
        }

        sample.valid = result == ESP_OK;
        sample.sampledAt = xTaskGetTickCount();
        xQueueOverwrite(localTemperatureQueue, &sample);
        char timestamp[32];
        rtc_clock_timestamp(timestamp, sizeof(timestamp));
        if (sample.valid) {
            ESP_LOGI(LOCAL_TAG, "%s | GPIO%d | Temperatura=%.2f C",
                     timestamp, DS18B20_GPIO, (double)(sample.temperatureC));
        } else {
            ESP_LOGW(LOCAL_TAG, "%s | Sensor indisponivel no GPIO%d: %s; tentando novamente",
                     timestamp, DS18B20_GPIO, esp_err_to_name(result));
            if (sensor) {
                ds18b20_del_device(sensor);
                sensor = NULL;
            }
        }
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(LOCAL_SAMPLE_PERIOD_MS));
    }
}

bool local_temperature_create_queue(void) {
    localTemperatureQueue = xQueueCreate(1, sizeof(LocalTemperature));
    return localTemperatureQueue != NULL;
}

bool local_temperature_latest(LocalTemperature *sample) {
    return sample && localTemperatureQueue && xQueuePeek(localTemperatureQueue, sample, 0) == pdTRUE;
}

void local_temperature_start(void) {
    if (xTaskCreate(localTemperatureTask, "ds18b20_local",
                    4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(LOCAL_TAG, "Falha ao criar tarefa; sensor local indisponivel");
    }

}
