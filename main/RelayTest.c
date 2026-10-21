#include "RelayTest.h"
#include "RelayConfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RELAY";
static const int OFF = RELAY_ACTIVE_LOW ? 1 : 0;
static const int ON = RELAY_ACTIVE_LOW ? 0 : 1;

static void allOff(void) {
    ESP_ERROR_CHECK(gpio_set_level(RELAY_HEATER, OFF));
    ESP_ERROR_CHECK(gpio_set_level(RELAY_COOLER, OFF));
}

static void testTask(void *context) {
    (void)context;
    ESP_LOGI(TAG, "Teste continuo em %u ms: mantenha as cargas desconectadas dos contatos",
             RELAY_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(RELAY_START_DELAY_MS));
    for (;;) {
        // Desliga o anterior antes de ligar o proximo.
        allOff();
        ESP_ERROR_CHECK(gpio_set_level(RELAY_HEATER, ON));
        ESP_LOGI(TAG, "AQUECEDOR ligado; REFRIGERADOR desligado (%u ms)",
                 RELAY_ON_TIME_MS);
        vTaskDelay(pdMS_TO_TICKS(RELAY_ON_TIME_MS));
        allOff();
        ESP_ERROR_CHECK(gpio_set_level(RELAY_COOLER, ON));
        ESP_LOGI(TAG, "AQUECEDOR desligado; REFRIGERADOR ligado (%u ms)",
                 RELAY_ON_TIME_MS);
        vTaskDelay(pdMS_TO_TICKS(RELAY_ON_TIME_MS));
    }
}

void relay_test_start(void) {
    static bool initialized = false;
    if (initialized) return;
    // Precarrega o nivel desligado antes de habilitar as saidas.
    allOff();
    gpio_config_t config = {0};
    config.pin_bit_mask = (1ULL << RELAY_HEATER) |
                          (1ULL << RELAY_COOLER);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = RELAY_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = RELAY_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));
    allOff();
    initialized = true;
    ESP_LOGI(TAG, "Aquecedor=GPIO%d refrigerador=GPIO%d ativo em %s; ambos desligados",
             (int)RELAY_HEATER, (int)RELAY_COOLER,
             RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
    if (RELAY_TEST_ENABLED &&
        xTaskCreate(testTask, "relay_test", 2048, NULL, 1, NULL) != pdPASS) {
        allOff();
        ESP_LOGE(TAG, "Falha ao criar tarefa de teste; ambos desligados");
    }
}
