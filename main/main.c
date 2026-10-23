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
#include "RelayTest.h"
#include "UbidotsTelemetry.h"

static const char *TAG = "ISPINDEL";

void app_main(void) {
    relay_test_start();
    // Nao apaga automaticamente configuracoes existentes.
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_station_initialize();

    // Preserva a ordem de criacao das duas filas, mesmo em caso de falha.
    const bool rxQueueReady = ispindel_receiver_create_queue();
    const bool localQueueReady = local_temperature_create_queue();
    if (!rxQueueReady || !localQueueReady) {
        ESP_LOGE(TAG, "Falha ao criar fila");
        abort();
    }

    rtc_clock_start();
    ispindel_receiver_start();
    local_temperature_start();
    display_test_start();
    ubidots_telemetry_start();

    ESP_LOGI(TAG, "Receptor Wi-Fi/HTTP pronto; porta %u", BREW_HTTP_PORT);
}
