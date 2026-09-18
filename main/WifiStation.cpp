#include "WifiStation.h"
#include "BrewProtocol.h"

#include <cstdlib>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace wifi_station {

static const char *TAG = "ISPINDEL";

void initialize() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "Falha ao criar interface STA");
        std::abort();
    }

    wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Teste sem associação com roteador.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(
        esp_wifi_set_channel(BREW_CHANNEL, WIFI_SECOND_CHAN_NONE)
    );

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));

    ESP_LOGI(TAG,
             "MAC STA RECEPTOR: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

}

} // namespace wifi_station
