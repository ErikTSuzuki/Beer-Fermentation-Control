#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "BrewProtocol.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "WifiStation.h"
#include "DisplayTest.h"
#include "RtcClock.h"
#include "TemperatureControl.h"
#include "FermentationRecipe.h"
#include "CaptivePortal.h"
#include "UbidotsTelemetry.h"

static const char *TAG = "ISPINDEL";

void app_main(void) {
    ESP_ERROR_CHECK(temperature_control_initialize());
    // Nao apaga automaticamente configuracoes existentes.
    ESP_ERROR_CHECK(nvs_flash_init());

    // Inicia os servicos Wi-Fi sem esperar conexao ao roteador ou a nuvem.
    wifi_station_initialize();

    // Preserva a ordem de criacao das duas filas, mesmo em caso de falha.
    const bool rxQueueReady = ispindel_receiver_create_queue();
    const bool localQueueReady = local_temperature_create_queue();
    if (!rxQueueReady || !localQueueReady) {
        ESP_LOGE(TAG, "Falha ao criar fila");
        abort();
    }

    rtc_clock_start();
    local_temperature_start();
    ESP_ERROR_CHECK(fermentation_recipe_start());
    ESP_ERROR_CHECK(temperature_control_start());
    ispindel_receiver_start();
    const esp_err_t portalResult = captive_portal_start();
    if (portalResult != ESP_OK)
        ESP_LOGW(TAG, "Portal automatico indisponivel: %s; acesso direto continua ativo",
                 esp_err_to_name(portalResult));
    display_test_start();
    ubidots_telemetry_start();

    ESP_LOGI(TAG, "Receptor Wi-Fi/HTTP pronto; porta %u", BREW_HTTP_PORT);
}
