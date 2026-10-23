#include "DisplayTest.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "RtcClock.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DISPLAY";
static const uint32_t LOCAL_STALE_MS = 5000;
static const uint16_t WHITE = 0xFFFF;
static const uint16_t RED = 0xF800;
enum { COLUMNS = 24 };

// Reescreve apenas linhas alteradas e apaga restos de valores anteriores.
typedef struct {
    int y;
    char previous[COLUMNS + 1];
    uint16_t previousColor;
} Line;

static void showLine(Line *line, const char *value, uint16_t color) {
    char padded[COLUMNS + 1];
    snprintf(padded, sizeof(padded), "%-24.24s", value);
    if (color == line->previousColor && strcmp(line->previous, padded) == 0) return;
    mar2406_display_text(16, line->y, padded, color, 0x0000, 2);
    memcpy(line->previous, padded, sizeof(line->previous));
    line->previousColor = color;
}

static const char *gravityUnit(uint8_t unit) {
    if (unit == BREW_UNIT_SG) return "SG";
    if (unit == BREW_UNIT_PLATO) return "PLATO";
    return "SEM UNID";
}

static void displayTask(void *context) {
    (void)context;
    const esp_err_t result = mar2406_display_initialize();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar tela: %s", esp_err_to_name(result));
        vTaskDelete(NULL);
        return;
    }

    mar2406_display_fill(0x0000);
    mar2406_display_text(16, 12, "BEER MONITOR", 0x07FF, 0x0000, 2);
    Line clock = {.y = 36};
    mar2406_display_text(16, 60, "DS18B20 LOCAL", 0xFFE0, 0x0000, 2);
    mar2406_display_text(16, 100, "ISPINDEL", 0xFFE0, 0x0000, 2);
    Line local = {.y = 80};
    Line remoteStatus = {.y = 116};
    Line remoteTemp = {.y = 140};
    Line gravity = {.y = 164};
    Line battery = {.y = 188};
    Line tilt = {.y = 212};
    ESP_LOGI(TAG, "Monitor de sensores iniciado");

    for (;;) {
        char line[64];
        const bool clockOk = rtc_clock_timestamp(line, sizeof(line));
        showLine(&clock, line, clockOk ? WHITE : RED);
        LocalTemperature sample = {0};
        const bool haveLocal = local_temperature_latest(&sample);
        const bool localOk = haveLocal && sample.valid &&
            xTaskGetTickCount() - sample.sampledAt <= pdMS_TO_TICKS(LOCAL_STALE_MS);
        if (localOk) {
            snprintf(line, sizeof(line), "TEMP: %.2f C", (double)(sample.temperatureC));
            showLine(&local, line, WHITE);
        } else {
            showLine(&local, "ERRO: NAO ENCONTRADO", RED);
        }

        IspindelReading reading = {0};
        const bool haveRemote = ispindel_receiver_latest(&reading);
        const bool remoteOk = haveRemote && xTaskGetTickCount() - reading.receivedAt <=
            ISPINDEL_STALE_AFTER_TICKS;
        if (remoteOk) {
            const BrewTelemetry *p = &reading.packet;
            showLine(&remoteStatus, "STATUS: RECEBENDO", 0x07E0);
            snprintf(line, sizeof(line), "TEMP: %.2f C", (double)(p->temperatureC));
            showLine(&remoteTemp, line, WHITE);
            snprintf(line, sizeof(line), "DENS: %.4f %s", (double)(p->gravity),
                          gravityUnit(p->header.gravityUnit));
            showLine(&gravity, line, WHITE);
            snprintf(line, sizeof(line), "BATERIA: %.2f V", (double)(p->batteryV));
            showLine(&battery, line, WHITE);
            snprintf(line, sizeof(line), "INCLINACAO: %.2f GRAUS", (double)(p->tiltDegrees));
            showLine(&tilt, line, WHITE);
        } else {
            showLine(&remoteStatus, "ERRO: NAO ENCONTRADO", RED);
            showLine(&remoteTemp, "TEMP: -- C", WHITE);
            showLine(&gravity, "DENS: --", WHITE);
            showLine(&battery, "BATERIA: -- V", WHITE);
            showLine(&tilt, "INCLINACAO: --", WHITE);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void display_test_start(void) {
    if (!MAR2406_TEST_ENABLED) return;
    if (xTaskCreate(displayTask, "mar2406_monitor", 4096,
                    NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa da tela");
    }
}
