#include "WifiStation.h"
#include "BrewProtocol.h"
#include "CaptivePortal.h"

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
#include "esp_eap_client.h"
#endif

static const char *TAG = "ISPINDEL";
static EventGroupHandle_t wifiEvents;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_ASSOCIATED_BIT BIT1
#define WIFI_CONNECTING_BIT BIT2
#define WIFI_DISCONNECTED_BIT BIT3
#define WIFI_RECONFIGURE_BIT BIT4

bool wifi_station_is_connected(void) {
    return wifiEvents &&
        (xEventGroupGetBits(wifiEvents) & WIFI_CONNECTED_BIT) != 0;
}

#if CONFIG_BEER_WIFI_UPLINK_ENABLED
typedef struct {
    char ssid[33], password[128], username[128], identity[128];
} UplinkCredentials;

typedef struct {
    UplinkCredentials credentials;
    TickType_t queuedAt;
    TickType_t delay;
} UplinkChange;

static QueueHandle_t uplinkChanges;
static SemaphoreHandle_t uplinkLock;
static const char *const UPLINK_NAMESPACE = "wifi_uplink";

static bool credentialsValid(const char *ssid, const char *password,
                             const char *username, const char *identity) {
    const size_t ssidLength = strlen(ssid), passwordLength = strlen(password);
    if (!ssidLength || ssidLength > 32 || !strcmp(ssid, CONFIG_BEER_AP_SSID)) return false;
    if (username[0])
        return wifi_station_supports_enterprise() && strlen(username) <= 127 &&
            strlen(identity) <= 127 && passwordLength >= 1 && passwordLength <= 127;
    if (identity[0]) return false;
    return !passwordLength || (passwordLength >= 8 && passwordLength <= 63) ||
        (passwordLength == 64 && strspn(password, "0123456789abcdefABCDEF") == 64);
}

static void copyCredentials(UplinkCredentials *credentials, const char *ssid,
                            const char *password, const char *username, const char *identity) {
    *credentials = (UplinkCredentials){0};
    strcpy(credentials->ssid, ssid);
    strcpy(credentials->password, password);
    strcpy(credentials->username, username);
    strcpy(credentials->identity, identity);
}

static bool loadBootCredentials(UplinkCredentials *credentials) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(UPLINK_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        size_t size = sizeof(*credentials);
        result = nvs_get_blob(handle, "credentials", credentials, &size);
        nvs_close(handle);
        if (result == ESP_OK)
            return size == sizeof(*credentials) &&
                memchr(credentials->ssid, 0, sizeof(credentials->ssid)) &&
                memchr(credentials->password, 0, sizeof(credentials->password)) &&
                memchr(credentials->username, 0, sizeof(credentials->username)) &&
                memchr(credentials->identity, 0, sizeof(credentials->identity)) &&
                credentialsValid(credentials->ssid, credentials->password,
                                 credentials->username, credentials->identity);
    }
    if (result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao ler rede externa salva: %s", esp_err_to_name(result));
        return false;
    }
    const char *ssid = CONFIG_BEER_WIFI_SSID, *password = CONFIG_BEER_WIFI_PASSWORD;
#if CONFIG_BEER_WIFI_ENTERPRISE
    const char *username = CONFIG_BEER_WIFI_USERNAME, *identity = CONFIG_BEER_WIFI_IDENTITY;
    if (!username[0]) return false;
#else
    const char *username = "", *identity = "";
#endif
    if (!credentialsValid(ssid, password, username, identity)) return false;
    copyCredentials(credentials, ssid, password, username, identity);
    return true;
}

#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
static esp_err_t configureEnterprise(const UplinkCredentials *credentials) {
    const char *username = credentials->username;
    const char *identity = credentials->identity[0] ? credentials->identity : username;
    const char *password = credentials->password;
    esp_err_t result = esp_eap_client_set_identity(
        (const unsigned char *)identity, (int)strlen(identity));
    if (result == ESP_OK)
        result = esp_eap_client_set_username((const unsigned char *)username, (int)strlen(username));
    if (result == ESP_OK)
        result = esp_eap_client_set_password((const unsigned char *)password, (int)strlen(password));
    if (result == ESP_OK) result = esp_wifi_sta_enterprise_enable();
    if (result != ESP_OK) (void)esp_wifi_sta_enterprise_disable();
    return result;
}
#endif

static esp_err_t applyCredentials(const UplinkCredentials *credentials) {
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    if (credentials->username[0]) {
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    } else {
        memcpy(config.sta.password, credentials->password, strlen(credentials->password));
        if (credentials->password[0]) config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }
    // AP e STA compartilham o radio e acompanham o canal da rede externa.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    static bool enterpriseEnabled;
    if (enterpriseEnabled) {
        const esp_err_t result = esp_wifi_sta_enterprise_disable();
        if (result != ESP_OK) return result;
        enterpriseEnabled = false;
    }
#endif
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    if (result == ESP_OK && credentials->username[0]) {
        result = configureEnterprise(credentials);
        enterpriseEnabled = result == ESP_OK;
    }
#endif
    return result;
}

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
            xEventGroupClearBits(wifiEvents, WIFI_ASSOCIATED_BIT | WIFI_CONNECTING_BIT);
            xEventGroupSetBits(wifiEvents, WIFI_DISCONNECTED_BIT);
            ESP_LOGW(TAG, "Wi-Fi desconectado (motivo %u); nova tentativa em 30 s",
                     event->reason);
        }
    }
}

static void disconnectUplink(void) {
    const EventBits_t active = WIFI_ASSOCIATED_BIT | WIFI_CONNECTING_BIT;
    xEventGroupClearBits(wifiEvents, WIFI_DISCONNECTED_BIT);
    if ((xEventGroupGetBits(wifiEvents) & active) && esp_wifi_disconnect() == ESP_OK)
        (void)xEventGroupWaitBits(wifiEvents, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE,
                                 pdMS_TO_TICKS(2000));
    xEventGroupClearBits(wifiEvents, active | WIFI_CONNECTED_BIT);
}

static void wifiConnectTask(void *context) {
    (void)context;
    UplinkCredentials credentials;
    bool apply = loadBootCredentials(&credentials);
    bool configured = false;
    for (;;) {
        if (apply) {
            const esp_err_t result = applyCredentials(&credentials);
            configured = result == ESP_OK;
            if (!configured)
                ESP_LOGW(TAG, "Falha ao configurar rede externa: %s", esp_err_to_name(result));
            apply = false;
        }
        if (configured && !(xEventGroupGetBits(wifiEvents) &
                           (WIFI_ASSOCIATED_BIT | WIFI_CONNECTING_BIT))) {
            xEventGroupClearBits(wifiEvents, WIFI_DISCONNECTED_BIT);
            xEventGroupSetBits(wifiEvents, WIFI_CONNECTING_BIT);
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                xEventGroupClearBits(wifiEvents, WIFI_CONNECTING_BIT);
                ESP_LOGW(TAG, "Falha ao conectar rede externa: %s", esp_err_to_name(result));
            }
        }
        EventBits_t events = xEventGroupWaitBits(
            wifiEvents, WIFI_DISCONNECTED_BIT | WIFI_RECONFIGURE_BIT, pdTRUE, pdFALSE,
            configured ? pdMS_TO_TICKS(30000) : portMAX_DELAY);
        if ((events & WIFI_DISCONNECTED_BIT) && !(events & WIFI_RECONFIGURE_BIT))
            events = xEventGroupWaitBits(wifiEvents, WIFI_RECONFIGURE_BIT, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(30000));
        if (events & WIFI_RECONFIGURE_BIT) {
            UplinkChange change;
            if (xQueueReceive(uplinkChanges, &change, 0) == pdTRUE) {
                // Respeita o prazo da ultima configuracao, mesmo com pedidos simultaneos.
                for (;;) {
                    const TickType_t elapsed = xTaskGetTickCount() - change.queuedAt;
                    if (elapsed >= change.delay ||
                        xQueueReceive(uplinkChanges, &change, change.delay - elapsed) != pdTRUE) break;
                }
                credentials = change.credentials;
                disconnectUplink();
                apply = true;
            }
        }
    }
}
#endif

bool wifi_station_can_configure(void) {
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
    return uplinkChanges != NULL;
#else
    return false;
#endif
}

bool wifi_station_supports_enterprise(void) {
#if CONFIG_BEER_WIFI_UPLINK_ENABLED && CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    return true;
#else
    return false;
#endif
}

static esp_err_t setUplink(const char *ssid, const char *password,
                          const char *username, const char *identity, unsigned delayMs) {
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
    if (!uplinkChanges) return ESP_ERR_INVALID_STATE;
    if (!credentialsValid(ssid, password, username, identity)) return ESP_ERR_INVALID_ARG;

    UplinkChange change = {.delay = pdMS_TO_TICKS(delayMs)};
    copyCredentials(&change.credentials, ssid, password, username, identity);
    nvs_handle_t handle;
    xSemaphoreTake(uplinkLock, portMAX_DELAY);
    esp_err_t result = nvs_open(UPLINK_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "credentials", &change.credentials, sizeof(change.credentials));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) {
        change.queuedAt = xTaskGetTickCount();
        xQueueOverwrite(uplinkChanges, &change);
        xEventGroupSetBits(wifiEvents, WIFI_RECONFIGURE_BIT);
    }
    xSemaphoreGive(uplinkLock);
    return result;
#else
    (void)ssid; (void)password; (void)username; (void)identity; (void)delayMs;
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t wifi_station_set_uplink(const char *ssid, const char *password,
                                 const char *username, const char *identity) {
    return setUplink(ssid, password, username, identity, 300);
}

esp_err_t wifi_station_set_uplink_remote(const char *ssid, const char *password,
                                        const char *username, const char *identity) {
    return setUplink(ssid, password, username, identity, 2500);
}

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
    uplinkChanges = xQueueCreate(1, sizeof(UplinkChange));
    uplinkLock = uplinkChanges ? xSemaphoreCreateMutex() : NULL;
    if (!uplinkLock || xTaskCreate(wifiConnectTask, "wifi_connect", 4096, NULL, 2, NULL) != pdPASS) {
        if (uplinkChanges) vQueueDelete(uplinkChanges);
        if (uplinkLock) vSemaphoreDelete(uplinkLock);
        uplinkChanges = NULL;
        uplinkLock = NULL;
    }
    if (!uplinkChanges) ESP_LOGE(TAG, "Falha ao criar tarefa Wi-Fi; mantendo modo local");
#endif
}
