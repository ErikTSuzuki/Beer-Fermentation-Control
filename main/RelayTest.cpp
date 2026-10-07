#include "RelayTest.h"
#include "RelayConfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "RELAY";
constexpr int OFF = relay_config::ACTIVE_LOW ? 1 : 0;
constexpr int ON = relay_config::ACTIVE_LOW ? 0 : 1;

void allOff() {
    ESP_ERROR_CHECK(gpio_set_level(relay_config::HEATER, OFF));
    ESP_ERROR_CHECK(gpio_set_level(relay_config::COOLER, OFF));
}

void testTask(void *) {
    ESP_LOGI(TAG, "Teste continuo em %u ms: mantenha as cargas desconectadas dos contatos",
             relay_config::START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(relay_config::START_DELAY_MS));
    for (;;) {
        // Desliga o anterior antes de ligar o proximo.
        allOff();
        ESP_ERROR_CHECK(gpio_set_level(relay_config::HEATER, ON));
        ESP_LOGI(TAG, "AQUECEDOR ligado; REFRIGERADOR desligado (%u ms)",
                 relay_config::ON_TIME_MS);
        vTaskDelay(pdMS_TO_TICKS(relay_config::ON_TIME_MS));
        allOff();
        ESP_ERROR_CHECK(gpio_set_level(relay_config::COOLER, ON));
        ESP_LOGI(TAG, "AQUECEDOR desligado; REFRIGERADOR ligado (%u ms)",
                 relay_config::ON_TIME_MS);
        vTaskDelay(pdMS_TO_TICKS(relay_config::ON_TIME_MS));
    }
}
} // namespace

namespace relay_test {
void start() {
    static bool initialized = false;
    if (initialized) return;
    // Precarrega o nivel desligado antes de habilitar as saidas.
    allOff();
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << relay_config::HEATER) |
                          (1ULL << relay_config::COOLER);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = relay_config::ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = relay_config::ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    allOff();
    initialized = true;
    ESP_LOGI(TAG, "Aquecedor=GPIO%d refrigerador=GPIO%d ativo em %s; ambos desligados",
             int(relay_config::HEATER), int(relay_config::COOLER),
             relay_config::ACTIVE_LOW ? "LOW" : "HIGH");
    if (relay_config::TEST_ENABLED &&
        xTaskCreate(testTask, "relay_test", 2048, nullptr, 1, nullptr) != pdPASS) {
        allOff();
        ESP_LOGE(TAG, "Falha ao criar tarefa de teste; ambos desligados");
    }
}
} // namespace relay_test
