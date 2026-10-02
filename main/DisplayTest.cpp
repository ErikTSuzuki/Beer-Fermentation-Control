#include "DisplayTest.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"

#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "DISPLAY";
constexpr uint32_t LOCAL_STALE_MS = 5000;
constexpr uint16_t WHITE = 0xFFFF;
constexpr uint16_t RED = 0xF800;
constexpr int COLUMNS = 24;

// Reescreve apenas linhas alteradas e apaga restos de valores anteriores.
struct Line {
    int y;
    char previous[COLUMNS + 1] = {};
    uint16_t previousColor = 0;

    void show(const char *value, uint16_t color = WHITE) {
        char padded[COLUMNS + 1];
        std::snprintf(padded, sizeof(padded), "%-24.24s", value);
        if (color == previousColor && std::strcmp(previous, padded) == 0) return;
        mar2406_display::text(16, y, padded, color);
        std::memcpy(previous, padded, sizeof(previous));
        previousColor = color;
    }
};

const char *gravityUnit(uint8_t unit) {
    if (unit == BREW_UNIT_SG) return "SG";
    if (unit == BREW_UNIT_PLATO) return "PLATO";
    return "SEM UNID";
}

void displayTask(void *) {
    const esp_err_t result = mar2406_display::initialize();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar tela: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    using mar2406_display::text;
    mar2406_display::fill(0x0000);
    text(16, 12, "BEER MONITOR", 0x07FF);
    text(16, 42, "DS18B20 LOCAL", 0xFFE0);
    text(16, 94, "ISPINDEL", 0xFFE0);
    Line local{64};
    Line remoteStatus{116};
    Line remoteTemp{140};
    Line gravity{164};
    Line battery{188};
    Line tilt{212};
    ESP_LOGI(TAG, "Monitor de sensores iniciado");

    for (;;) {
        char line[64];
        LocalTemperature sample = {};
        const bool haveLocal = local_temperature::latest(sample);
        const bool localOk = haveLocal && sample.valid &&
            xTaskGetTickCount() - sample.sampledAt <= pdMS_TO_TICKS(LOCAL_STALE_MS);
        if (localOk) {
            std::snprintf(line, sizeof(line), "TEMP: %.2f C", double(sample.temperatureC));
            local.show(line);
        } else {
            local.show("ERRO: NAO ENCONTRADO", RED);
        }

        ispindel_receiver::Reading reading = {};
        const bool haveRemote = ispindel_receiver::latest(reading);
        const bool remoteOk = haveRemote && xTaskGetTickCount() - reading.receivedAt <=
            pdMS_TO_TICKS(ispindel_receiver::STALE_AFTER_MS);
        if (remoteOk) {
            const BrewTelemetry &p = reading.packet;
            remoteStatus.show("STATUS: RECEBENDO", 0x07E0);
            std::snprintf(line, sizeof(line), "TEMP: %.2f C", double(p.temperatureC));
            remoteTemp.show(line);
            std::snprintf(line, sizeof(line), "DENS: %.4f %s", double(p.gravity),
                          gravityUnit(p.header.gravityUnit));
            gravity.show(line);
            std::snprintf(line, sizeof(line), "BATERIA: %.2f V", double(p.batteryV));
            battery.show(line);
            std::snprintf(line, sizeof(line), "INCLINACAO: %.2f GRAUS", double(p.tiltDegrees));
            tilt.show(line);
        } else {
            remoteStatus.show("ERRO: NAO ENCONTRADO", RED);
            remoteTemp.show("TEMP: -- C");
            gravity.show("DENS: --");
            battery.show("BATERIA: -- V");
            tilt.show("INCLINACAO: --");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

} // namespace

namespace display_test {

void start() {
    if (!mar2406_config::TEST_ENABLED) return;
    if (xTaskCreate(displayTask, "mar2406_monitor", 4096,
                    nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa da tela");
    }
}

} // namespace display_test
