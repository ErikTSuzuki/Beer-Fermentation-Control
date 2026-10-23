#include "IspindelReceiver.h"
#include "TelemetryLogger.h"
#include "BrewJson.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

static const char *TAG = "ISPINDEL";
static QueueHandle_t latestQueue;
static httpd_handle_t server;
// Os handlers do servidor executam sequencialmente na mesma tarefa.
static bool selected;
static uint8_t sensorMac[6];
static bool havePrevious;
static BrewTelemetry previous;
static uint32_t count;

static esp_err_t localConnectionOnly(httpd_handle_t handle, int fd) {
    (void)handle;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info = {0};
    struct sockaddr_in local = {0};
    socklen_t length = sizeof(local);
    struct ifreq interface = {0};
    // ESP-IDF 5/6.0 nao oferece bind de interface em httpd_config_t.
    // Fecha conexoes pela interface externa antes de ler qualquer HTTP.
    if (!ap || esp_netif_get_ip_info(ap, &info) != ESP_OK ||
        getsockname(fd, (struct sockaddr *)&local, &length) < 0 ||
        local.sin_family != AF_INET || local.sin_addr.s_addr != info.ip.addr ||
        esp_netif_get_netif_impl_name(ap, interface.ifr_name) != ESP_OK ||
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool sourceStation(int fd, uint8_t mac[6], int *rssi) {
    struct sockaddr_in peer = {0};
    socklen_t length = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &length) < 0 ||
        peer.sin_family != AF_INET) return false;
    wifi_sta_list_t stations = {0};
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap || esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return false;
    for (int i = 0; i < stations.num; ++i) {
        esp_netif_pair_mac_ip_t client = {0};
        memcpy(client.mac, stations.sta[i].mac, sizeof(client.mac));
        if (esp_netif_dhcps_get_clients_by_mac(ap, 1, &client) == ESP_OK &&
            client.ip.addr != 0 && client.ip.addr == peer.sin_addr.s_addr) {
            memcpy(mac, client.mac, sizeof(client.mac));
            *rssi = stations.sta[i].rssi;
            return true;
        }
    }
    return false;
}

static esp_err_t reject(httpd_req_t *request, const char *status, const char *body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    // Fecha tambem quando o corpo rejeitado nao tiver sido consumido.
    return ESP_FAIL;
}

static esp_err_t receiveReading(httpd_req_t *request) {
    uint8_t mac[6];
    int rssi;
    if (!sourceStation(httpd_req_to_sockfd(request), mac, &rssi)) {
        return reject(request, "403 Forbidden", "{\"accepted\":false,\"error\":\"local_station_required\"}");
    }
    char contentType[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", contentType, sizeof(contentType)) != ESP_OK ||
        strncasecmp(contentType, "application/json", 16) != 0 ||
        (contentType[16] && contentType[16] != ';' && contentType[16] != ' ')) {
        return reject(request, "415 Unsupported Media Type", "{\"accepted\":false,\"error\":\"json_required\"}");
    }
    if (!request->content_len || request->content_len >= BREW_HTTP_BODY_MAX) {
        return reject(request, "413 Content Too Large", "{\"accepted\":false,\"error\":\"body_size\"}");
    }
    char body[BREW_HTTP_BODY_MAX];
    size_t received = 0;
    const TickType_t started = xTaskGetTickCount();
    while (received < request->content_len) {
        const int bytes = httpd_req_recv(request, body + received, request->content_len - received);
        if (bytes <= 0 || xTaskGetTickCount() - started > pdMS_TO_TICKS(3000)) {
            return reject(request, "408 Request Timeout", "{\"accepted\":false,\"error\":\"incomplete_body\"}");
        }
        received += (size_t)bytes;
    }
    BrewTelemetry packet;
    if (!brew_json_parse(body, received, &packet)) {
        return reject(request, "400 Bad Request", "{\"accepted\":false,\"error\":\"invalid_reading\"}");
    }
    if (selected && memcmp(sensorMac, mac, sizeof(sensorMac)) != 0) {
        return reject(request, "409 Conflict", "{\"accepted\":false,\"error\":\"another_sensor_selected\"}");
    }
    const bool duplicate = havePrevious &&
        previous.header.sensorId == packet.header.sensorId &&
        previous.header.bootId == packet.header.bootId &&
        previous.header.sequence == packet.header.sequence;
    if (duplicate && memcmp(&packet, &previous, sizeof(packet)) != 0) {
        return reject(request, "409 Conflict", "{\"accepted\":false,\"error\":\"sequence_conflict\"}");
    }
    if (!duplicate) {
        const IspindelReading reading = {packet, xTaskGetTickCount()};
        // Confirma somente depois de disponibilizar a leitura para tela/nuvem.
        xQueueOverwrite(latestQueue, &reading);
        memcpy(sensorMac, mac, sizeof(sensorMac));
        selected = true;
        previous = packet;
        havePrevious = true;
        telemetry_logger_print_reading(&packet, mac, rssi, ++count);
    }
    char response[192];
    snprintf(response, sizeof(response),
             "{\"accepted\":true,\"version\":%u,\"sensor_id\":%" PRIu32
             ",\"boot_id\":%" PRIu32 ",\"sequence\":%" PRIu32 "}",
             BREW_VERSION, packet.header.sensorId, packet.header.bootId, packet.header.sequence);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Connection", "close");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

bool ispindel_receiver_create_queue(void) {
    latestQueue = xQueueCreate(1, sizeof(IspindelReading));
    return latestQueue != NULL;
}

bool ispindel_receiver_latest(IspindelReading *reading) {
    return reading && latestQueue && xQueuePeek(latestQueue, reading, 0) == pdTRUE;
}

void ispindel_receiver_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = BREW_HTTP_PORT;
    config.stack_size = 6144;
    config.max_open_sockets = 2;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.lru_purge_enable = true;
    config.open_fn = localConnectionOnly;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t endpoint = {
        .uri = BREW_HTTP_PATH,
        .method = HTTP_POST,
        .handler = receiveReading,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &endpoint));
    ESP_LOGI(TAG, "Recepcao HTTP/JSON ativa: POST %s:%u%s",
             CONFIG_BEER_AP_IP, BREW_HTTP_PORT, BREW_HTTP_PATH);
}
