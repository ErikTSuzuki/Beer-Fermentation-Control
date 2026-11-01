#include "WifiStation.h"
#include "BrewProtocol.h"
#include "CaptivePortal.h"

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if CONFIG_BEER_WIFI_ENTERPRISE
#include "esp_eap_client.h"
#endif

static const char *TAG = "ISPINDEL";
static EventGroupHandle_t wifiEvents;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_ASSOCIATED_BIT BIT1
#define WIFI_CONNECTING_BIT BIT2
#define WIFI_DISCONNECTED_BIT BIT3

bool wifi_station_is_connected(void) {
    return wifiEvents &&
        (xEventGroupGetBits(wifiEvents) & WIFI_CONNECTED_BIT) != 0;
}

#if CONFIG_BEER_WIFI_UPLINK_ENABLED
#if CONFIG_BEER_WIFI_ENTERPRISE
static esp_err_t configureEnterprise(void) {
    static bool configured = false;
    if (configured) return ESP_OK;

    const char *username = CONFIG_BEER_WIFI_USERNAME;
    const char *identity = strlen(CONFIG_BEER_WIFI_IDENTITY) ?
        CONFIG_BEER_WIFI_IDENTITY : username;
    const char *password = CONFIG_BEER_WIFI_PASSWORD;
    esp_err_t result = esp_eap_client_set_identity(
        (const unsigned char *)identity, (int)strlen(identity));
    if (result == ESP_OK) {
        result = esp_eap_client_set_username(
            (const unsigned char *)username, (int)strlen(username));
    }
    if (result == ESP_OK) {
        result = esp_eap_client_set_password(
            (const unsigned char *)password, (int)strlen(password));
    }
    if (result == ESP_OK) result = esp_wifi_sta_enterprise_enable();
    if (result == ESP_OK) {
        configured = true;
    } else {
        // Libera configuracao parcial para permitir nova tentativa limpa.
        esp_wifi_sta_enterprise_disable();
    }
    return result;
}
#endif

static void onWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = data;
        xEventGroupSetBits(wifiEvents, WIFI_ASSOCIATED_BIT);
        xEventGroupClearBits(wifiEvents, WIFI_CONNECTING_BIT);
        ESP_LOGI(TAG, "Rede externa associada; SoftAP acompanha o canal %u", event->channel);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (xEventGroupGetBits(wifiEvents) & WIFI_ASSOCIATED_BIT) {
            xEventGroupSetBits(wifiEvents, WIFI_CONNECTED_BIT);
            wifi_ap_record_t ap = {0};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi conectado com IP no canal %u", ap.primary);
            }
        }
    } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        xEventGroupClearBits(wifiEvents, WIFI_CONNECTED_BIT);
        if (base == WIFI_EVENT) {
            const wifi_event_sta_disconnected_t *event = data;
            // Mantem o SoftAP ativo no canal atual ate a proxima associacao.
            xEventGroupSetBits(wifiEvents, WIFI_DISCONNECTED_BIT);
            xEventGroupClearBits(wifiEvents, WIFI_ASSOCIATED_BIT | WIFI_CONNECTING_BIT);
            ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %u); nova tentativa em 30 s",
                     event->reason);
        }
    }
}

static void wifiConnectTask(void *context) {
    (void)context;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, CONFIG_BEER_WIFI_SSID, strlen(CONFIG_BEER_WIFI_SSID));
#if CONFIG_BEER_WIFI_ENTERPRISE
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
#else
    memcpy(config.sta.password, CONFIG_BEER_WIFI_PASSWORD,
           strlen(CONFIG_BEER_WIFI_PASSWORD));
#endif
    // O driver descobre o canal. Nunca forcar canal enquanto houver clientes
    // no SoftAP: AP e STA compartilham o radio e seguem a associacao externa.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
#if CONFIG_BEER_WIFI_ENTERPRISE
    if (configureEnterprise() != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
#endif
    for (;;) {
        if (!(xEventGroupGetBits(wifiEvents) &
              (WIFI_ASSOCIATED_BIT | WIFI_CONNECTING_BIT))) {
            xEventGroupClearBits(wifiEvents, WIFI_DISCONNECTED_BIT);
            xEventGroupSetBits(wifiEvents, WIFI_CONNECTING_BIT);
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                xEventGroupClearBits(wifiEvents, WIFI_CONNECTING_BIT);
                ESP_LOGW(TAG, "Falha ao conectar rede externa: %s",
                         esp_err_to_name(result));
            }
        }
        const EventBits_t events = xEventGroupWaitBits(
            wifiEvents, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(30000));
        if (events & WIFI_DISCONNECTED_BIT) {
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
}
#endif

void wifi_station_initialize(void) {
    wifiEvents = xEventGroupCreate();
    if (!wifiEvents) abort();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "Falha ao criar interface STA");
        abort();
    }

    esp_netif_t *apNetif = esp_netif_create_default_wifi_ap();
    if (!apNetif) abort();
    // Sub-rede local configuravel para evitar sobreposicao com o roteador.
    esp_netif_ip_info_t local = {0};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_BEER_AP_IP, &local.ip));
    local.gw = local.ip;
    ESP_ERROR_CHECK(esp_netif_str_to_ip4("255.255.255.0", &local.netmask));
    const esp_err_t stopped = esp_netif_dhcps_stop(apNetif);
    if (stopped != ESP_OK && stopped != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(stopped);
    }
    ESP_ERROR_CHECK(esp_netif_set_ip_info(apNetif, &local));
    const esp_err_t portalDhcp = captive_portal_configure_dhcp(apNetif);
    if (portalDhcp != ESP_OK)
        ESP_LOGW(TAG, "DNS do portal indisponivel: %s; use http://%s/",
                 esp_err_to_name(portalDhcp), CONFIG_BEER_AP_IP);
    ESP_ERROR_CHECK(esp_netif_dhcps_start(apNetif));

    const size_t apSsidLength = strlen(CONFIG_BEER_AP_SSID);
    const size_t apPasswordLength = strlen(CONFIG_BEER_AP_PASSWORD);
    if (!apSsidLength || apSsidLength > 32 ||
        apPasswordLength < 8 || apPasswordLength > 63) {
        ESP_LOGE(TAG, "SoftAP requer SSID de 1-32 bytes e senha de 8-63 bytes");
        abort();
    }
    wifi_config_t apConfig = {0};
    memcpy(apConfig.ap.ssid, CONFIG_BEER_AP_SSID, apSsidLength);
    memcpy(apConfig.ap.password, CONFIG_BEER_AP_PASSWORD, apPasswordLength);
    apConfig.ap.ssid_len = apSsidLength;
    apConfig.ap.channel = BREW_CHANNEL;
    apConfig.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apConfig.ap.max_connection = 4; // iSpindel e ate tres dispositivos de configuracao.
    apConfig.ap.pmf_cfg.required = false; // Compatibilidade com ESP8266.

    wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              onWifiEvent, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              onWifiEvent, NULL));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "SoftAP %s ativo em %s; HTTP %u; canal inicial %u",
             CONFIG_BEER_AP_SSID, CONFIG_BEER_AP_IP, BREW_HTTP_PORT, BREW_CHANNEL);

#if CONFIG_BEER_WIFI_UPLINK_ENABLED
    const size_t ssidLength = strlen(CONFIG_BEER_WIFI_SSID);
    const size_t passwordLength = strlen(CONFIG_BEER_WIFI_PASSWORD);
    if (!ssidLength) {
        ESP_LOGW(TAG, "Sem SSID externo: somente rede local do iSpindel ativa");
        return;
    }
    if (!strcmp(CONFIG_BEER_WIFI_SSID, CONFIG_BEER_AP_SSID)) {
        ESP_LOGE(TAG, "SSID externo deve ser diferente do SSID local");
        return;
    }
    if (ssidLength > 32) {
        ESP_LOGE(TAG, "SSID excede 32 bytes; mantendo modo local");
        return;
    }
#if CONFIG_BEER_WIFI_ENTERPRISE
    const size_t usernameLength = strlen(CONFIG_BEER_WIFI_USERNAME);
    const size_t identityLength = strlen(CONFIG_BEER_WIFI_IDENTITY);
    if (!usernameLength || usernameLength > 127 || identityLength > 127 || !passwordLength) {
        ESP_LOGE(TAG, "Enterprise requer usuario (1-127 bytes), senha e identidade de ate 127 bytes");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi: modo WPA2-Enterprise (usuario/senha)");
#else
    if (passwordLength > 64) {
        ESP_LOGE(TAG, "SSID/senha excedem o limite do Wi-Fi; mantendo modo local");
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi: modo Personal (senha da rede)");
#endif
    if (xTaskCreate(wifiConnectTask, "wifi_connect", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa Wi-Fi; mantendo modo local");
    }
#endif

}
