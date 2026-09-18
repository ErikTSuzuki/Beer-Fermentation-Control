#include <cstdlib>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "BrewProtocol.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "WifiStation.h"

static const char *TAG = "ISPINDEL";

extern "C" void app_main(void) {
    // Nao apaga automaticamente configuracoes existentes.
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_station::initialize();

    // Preserva a ordem de criacao das duas filas, mesmo em caso de falha.
    const bool rxQueueReady = ispindel_receiver::createQueue();
    const bool localQueueReady = local_temperature::createQueue();
    if (!rxQueueReady || !localQueueReady) {
        ESP_LOGE(TAG, "Falha ao criar fila");
        std::abort();
    }

    ispindel_receiver::start();
    local_temperature::start();

    ESP_LOGI(TAG, "Receptor pronto no canal %u", BREW_CHANNEL);
}
