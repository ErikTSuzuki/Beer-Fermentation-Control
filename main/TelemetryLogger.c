#include "TelemetryLogger.h"
#include "LocalTemperature.h"
#include "RtcClock.h"

#include <inttypes.h>
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "ISPINDEL";
static const uint32_t LOCAL_SAMPLE_MAX_AGE_MS = 5000;

static void printTemperatureComparison(float ispindelC, const char *timestamp) {
    LocalTemperature sample = {0};
    if (!local_temperature_latest(&sample) || !sample.valid) {
        ESP_LOGW(TAG, "%s | iSpindel=%.2f C | DS18B20 local=indisponivel",
                 timestamp, (double)(ispindelC));
        return;
    }

    TickType_t age = xTaskGetTickCount() - sample.sampledAt;
    if (age > pdMS_TO_TICKS(LOCAL_SAMPLE_MAX_AGE_MS)) {
        ESP_LOGW(TAG, "%s | iSpindel=%.2f C | DS18B20 local=leitura desatualizada",
                 timestamp, (double)(ispindelC));
        return;
    }

    ESP_LOGI(TAG,
             "%s | iSpindel=%.2f C | DS18B20 local=%.2f C | Delta(iSpindel-local)=%+.2f C"
             " | Idade local=%" PRIu32 " ms",
             timestamp, (double)(ispindelC),
             (double)(sample.temperatureC),
             (double)(ispindelC - sample.temperatureC),
             (uint32_t)(age * portTICK_PERIOD_MS));
}

static const char *gravityUnit(uint8_t unit) {
    switch (unit) {
        case BREW_UNIT_SG:    return "SG";
        case BREW_UNIT_PLATO: return "Plato";
        default:             return "unidade indefinida";
    }
}

void telemetry_logger_print_reading(const BrewTelemetry *p, const uint8_t *mac, int rssi, uint32_t count) {
    char timestamp[32];
    rtc_clock_timestamp(timestamp, sizeof(timestamp));
    ESP_LOGI(TAG,
             "%s | Leitura #%" PRIu32
             " | sensor=%06" PRIX32
             " | boot=%08" PRIX32
             " | seq=%" PRIu32,
             timestamp, count,
             p->header.sensorId,
             p->header.bootId,
             p->header.sequence);

    ESP_LOGI(TAG,
             "MAC=%02X:%02X:%02X:%02X:%02X:%02X | RSSI=%d dBm",
             mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5],
             rssi);

    ESP_LOGI(TAG,
             "%s | Temperatura=%.2f C | Inclinacao=%.2f graus",
             timestamp, (double)(p->temperatureC),
             (double)(p->tiltDegrees));

    ESP_LOGI(TAG,
             "%s | Densidade=%.4f %s | Bateria=%.3f V",
             timestamp, (double)(p->gravity),
             gravityUnit(p->header.gravityUnit),
             (double)(p->batteryV));

    printTemperatureComparison(p->temperatureC, timestamp);
}
