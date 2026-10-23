"""Exercise the actual HTTP handler with simulated ESP-IDF interfaces.

Run after test_hybrid_sender.py: python tests/test_http_receiver.py [path-to-gcc]
Generated files stay in build/hybrid-tests/receiver.
"""
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/hybrid-tests/receiver"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

MOCK = r'''
#ifndef SDK_MOCK_H
#define SDK_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef unsigned socklen_t;
typedef int esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { uint8_t mac[6]; esp_ip4_addr_t ip; } esp_netif_pair_mac_ip_t;
typedef struct { uint8_t mac[6]; int rssi; } wifi_sta_info_t;
typedef struct { int num; wifi_sta_info_t sta[1]; } wifi_sta_list_t;
struct sockaddr { int family; };
struct sockaddr_in { int sin_family; struct { uint32_t s_addr; } sin_addr; };
struct ifreq { char ifr_name[16]; };
typedef struct { size_t content_len; } httpd_req_t;
typedef struct {
    int server_port, stack_size, max_open_sockets, recv_wait_timeout, send_wait_timeout;
    bool lru_purge_enable;
    esp_err_t (*open_fn)(httpd_handle_t, int);
} httpd_config_t;
typedef struct {
    const char *uri; int method; esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define pdTRUE 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define CONFIG_BEER_ISPINDEL_STALE_SECONDS 1800
#define CONFIG_BEER_AP_IP "192.168.50.1"
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTP_POST 1
#define HTTPD_RESP_USE_STRLEN -1
#define AF_INET 2
#define SOL_SOCKET 1
#define SO_BINDTODEVICE 2
#define getsockname mock_getsockname
#define getpeername mock_getpeername
#define setsockopt mock_setsockopt
int mock_getsockname(int, struct sockaddr *, socklen_t *);
int mock_getpeername(int, struct sockaddr *, socklen_t *);
int mock_setsockopt(int, int, int, const void *, socklen_t);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
int esp_netif_get_netif_impl_name(esp_netif_t *, char *);
int esp_wifi_ap_get_sta_list(wifi_sta_list_t *);
int esp_netif_dhcps_get_clients_by_mac(esp_netif_t *, int, esp_netif_pair_mac_ip_t *);
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueOverwrite(QueueHandle_t, const void *);
int xQueuePeek(QueueHandle_t, void *, unsigned);
TickType_t xTaskGetTickCount(void);
int httpd_resp_set_status(httpd_req_t *, const char *);
int httpd_resp_set_type(httpd_req_t *, const char *);
int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
int httpd_resp_send(httpd_req_t *, const char *, int);
int httpd_req_to_sockfd(httpd_req_t *);
int httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
int httpd_start(httpd_handle_t *, const httpd_config_t *);
int httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
#endif
'''

HARNESS = r'''
#include "IspindelReceiver.c"
#include <stdlib.h>
static IspindelReading cached;
static int writes, logs, status, macValue = 1;
static uint32_t now, localIp = 1, peerIp = 2, dhcpIp = 2;
static bool associated = true, recvFail, sendFail, slow;
static const char *input, *contentType = "application/json";
static size_t offset;
static char response[256];
static esp_netif_t netif;
int mock_getsockname(int fd, struct sockaddr *out, socklen_t *length) {
    (void)fd; (void)length;
    struct sockaddr_in *addr = (struct sockaddr_in *)out;
    addr->sin_family = AF_INET; addr->sin_addr.s_addr = localIp; return 0;
}
int mock_getpeername(int fd, struct sockaddr *out, socklen_t *length) {
    (void)fd; (void)length;
    struct sockaddr_in *addr = (struct sockaddr_in *)out;
    addr->sin_family = AF_INET; addr->sin_addr.s_addr = peerIp; return 0;
}
int mock_setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    (void)fd; (void)length;
    assert(level == SOL_SOCKET && option == SO_BINDTODEVICE);
    assert(!strcmp(((const struct ifreq *)value)->ifr_name, "ap1")); return 0;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    assert(!strcmp(key, "WIFI_AP_DEF")); return &netif;
}
int esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *info) {
    (void)n; info->ip.addr = 1; return ESP_OK;
}
int esp_netif_get_netif_impl_name(esp_netif_t *n, char *name) {
    (void)n; strcpy(name, "ap1"); return ESP_OK;
}
int esp_wifi_ap_get_sta_list(wifi_sta_list_t *list) {
    list->num = associated ? 1 : 0;
    memset(list->sta[0].mac, macValue, 6); list->sta[0].rssi = -42; return ESP_OK;
}
int esp_netif_dhcps_get_clients_by_mac(esp_netif_t *n, int count, esp_netif_pair_mac_ip_t *client) {
    (void)n; assert(count == 1); client->ip.addr = dhcpIp; return ESP_OK;
}
QueueHandle_t xQueueCreate(unsigned length, unsigned size) {
    assert(length == 1 && size == sizeof(cached)); return &cached;
}
int xQueueOverwrite(QueueHandle_t queue, const void *item) {
    assert(queue == &cached); memcpy(&cached, item, sizeof(cached)); ++writes; return pdTRUE;
}
int xQueuePeek(QueueHandle_t queue, void *item, unsigned wait) {
    (void)queue; (void)wait;
    if (!writes) return 0;
    memcpy(item, &cached, sizeof(cached)); return pdTRUE;
}
TickType_t xTaskGetTickCount(void) { return now; }
void telemetry_logger_print_reading(const BrewTelemetry *p, const uint8_t *mac, int rssi, uint32_t count) {
    (void)p; assert(mac[0] == 1 && rssi == -42); assert(count == (uint32_t)++logs);
}
int httpd_resp_set_status(httpd_req_t *req, const char *value) {
    (void)req; status = atoi(value); return ESP_OK;
}
int httpd_resp_set_type(httpd_req_t *req, const char *type) {
    (void)req; assert(!strcmp(type, "application/json")); return ESP_OK;
}
int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value) {
    (void)req; assert(!strcmp(key, "Connection") && !strcmp(value, "close")); return ESP_OK;
}
int httpd_resp_send(httpd_req_t *req, const char *body, int size) {
    (void)req; assert(size == HTTPD_RESP_USE_STRLEN);
    assert(strlen(body) < sizeof(response)); strcpy(response, body); return sendFail ? ESP_FAIL : ESP_OK;
}
int httpd_req_to_sockfd(httpd_req_t *req) { (void)req; return 1; }
int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *out, size_t capacity) {
    (void)req; assert(!strcmp(key, "Content-Type"));
    if (!contentType || strlen(contentType) >= capacity) return ESP_FAIL;
    strcpy(out, contentType); return ESP_OK;
}
int httpd_req_recv(httpd_req_t *req, char *out, size_t length) {
    (void)req;
    if (recvFail) return -1;
    if (length > 13) length = 13; // Corpo fragmentado como num fluxo TCP real.
    memcpy(out, input + offset, length); offset += length;
    now += slow ? 4000 : 10; return (int)length;
}
int httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
    assert(config->server_port == 80 && config->max_open_sockets == 2);
    assert(config->open_fn == localConnectionOnly); *handle = &netif; return ESP_OK;
}
int httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri) {
    (void)handle; assert(!strcmp(uri->uri, "/ispindel") && uri->method == HTTP_POST);
    assert(uri->handler == receiveReading); return ESP_OK;
}
static int request(const char *body, size_t length) {
    input = body; offset = 0; status = 200; response[0] = 0;
    httpd_req_t req = {.content_len = length};
    return receiveReading(&req);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    char body[512]; size_t size = fread(body, 1, sizeof(body) - 1, file); fclose(file); body[size] = 0;
    int scenarios = 0;
    assert(ispindel_receiver_create_queue()); ispindel_receiver_start();
    IspindelReading reading;
    assert(!ispindel_receiver_latest(&reading)); ++scenarios;
    assert(localConnectionOnly(NULL, 1) == ESP_OK); ++scenarios;
    localIp = 3; assert(localConnectionOnly(NULL, 1) == ESP_FAIL); localIp = 1; ++scenarios;
    assert(request(body, size) == ESP_OK && status == 200 && writes == 1 && logs == 1);
    assert(strstr(response, "\"accepted\":true")); ++scenarios;
    assert(ispindel_receiver_latest(&reading));
    const TickType_t firstReceived = reading.receivedAt;
    now = 1000;
    assert(request(body, size) == ESP_OK && writes == 1 && logs == 1);
    assert(cached.receivedAt == firstReceived); ++scenarios;
    char conflict[512]; strcpy(conflict, body);
    char *temperature = strstr(conflict, "\"temperature_c\":"); assert(temperature);
    temperature[strlen("\"temperature_c\":")] = '3';
    assert(request(conflict, size) == ESP_FAIL && status == 409 && writes == 1); ++scenarios;
    macValue = 2; assert(request(body, size) == ESP_FAIL && status == 409); macValue = 1; ++scenarios;
    associated = false; assert(request(body, size) == ESP_FAIL && status == 403); associated = true; ++scenarios;
    peerIp = 9; assert(request(body, size) == ESP_FAIL && status == 403); peerIp = 2; ++scenarios;
    contentType = "text/plain"; assert(request(body, size) == ESP_FAIL && status == 415); ++scenarios;
    contentType = NULL; assert(request(body, size) == ESP_FAIL && status == 415); ++scenarios;
    contentType = "application/json; charset=utf-8";
    assert(request(body, size) == ESP_OK && writes == 1); ++scenarios;
    assert(request(body, 512) == ESP_FAIL && status == 413); ++scenarios;
    assert(request(body, 0) == ESP_FAIL && status == 413); ++scenarios;
    recvFail = true; assert(request(body, size) == ESP_FAIL && status == 408); recvFail = false; ++scenarios;
    slow = true; assert(request(body, size) == ESP_FAIL && status == 408); slow = false; ++scenarios;
    assert(request("{}", 2) == ESP_FAIL && status == 400 && writes == 1); ++scenarios;
    char *sequence = strstr(body, "\"sequence\":"); assert(sequence);
    sequence[strlen("\"sequence\":")] = '2';
    sendFail = true;
    assert(request(body, size) == ESP_FAIL && writes == 2 && logs == 2); ++scenarios;
    const TickType_t secondReceived = cached.receivedAt;
    sendFail = false;
    assert(request(body, size) == ESP_OK && writes == 2 && logs == 2);
    assert(cached.receivedAt == secondReceived); ++scenarios;
    printf("%d HTTP receiver scenarios passed\n", scenarios);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("Host gcc required; pass its path as an argument.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "sdk_mock.h").write_text(MOCK, encoding="utf-8")
    for name in [
        "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h", "sdkconfig.h",
        "esp_err.h", "esp_http_server.h", "esp_log.h", "esp_netif.h", "esp_wifi.h", "lwip/sockets.h",
    ]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "sdk_mock.h"\n', encoding="utf-8")
    source = BUILD / "receiver_test.c"
    source.write_text(HARNESS, encoding="utf-8")
    executable = BUILD / ("receiver_test.exe" if sys.platform == "win32" else "receiver_test")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DCJSON_NESTING_LIMIT=8",
        "-I" + str(BUILD), "-I" + str(PROJECT / "main"), "-I" + str(CJSON),
        str(source), str(PROJECT / "main/BrewJson.c"), str(CJSON / "cJSON.c"),
        "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([
        str(executable), str(BUILD.parent / "sender-sample.json"),
    ], check=True, timeout=10)


if __name__ == "__main__":
    main()
