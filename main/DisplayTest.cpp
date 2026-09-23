#include "DisplayTest.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"

#include <cstdio>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "DISPLAY_TEST";

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
    text(16, 38, "MAR2406 - TESTE", 0xFFFF);
    text(16, 66, "DADOS SIMULADOS", 0xFFE0);
    text(16, 98, "TEMP:     20.50 C", 0xFFFF);
    text(16, 124, "DENS:     1.048 SG", 0xFFFF);
    text(16, 150, "BATERIA:  3.85 V", 0xFFFF);
    text(16, 176, "STATUS:   TESTE ATIVO", 0x07E0);
    ESP_LOGI(TAG, "Demonstracao enviada; confira imagem e contador na tela");

    unsigned seconds = 0;
    for (;;) {
        char line[24];
        std::snprintf(line, sizeof(line), "CONTADOR: %05u", seconds);
        text(16, 210, line, 0xF81F);
        seconds = (seconds + 1) % 100000;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

} // namespace

namespace display_test {

void start() {
    if (!mar2406_config::TEST_ENABLED) return;
    if (xTaskCreate(displayTask, "mar2406_test", 4096,
                    nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa da tela");
    }
}

} // namespace display_test
