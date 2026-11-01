#include "CaptivePortal.h"
#include "CaptiveDns.h"
#include "sdkconfig.h"

#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "CAPTIVE";
static bool started;

esp_err_t captive_portal_configure_dhcp(esp_netif_t *ap) {
    // Evita configurar por engano o resolvedor global/STA usado pelo Ubidots.
    if (!ap || ap != esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) return ESP_ERR_INVALID_ARG;
    esp_netif_ip_info_t info;
    esp_err_t result = esp_netif_get_ip_info(ap, &info);
    if (result != ESP_OK) return result;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = info.ip;
    result = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    if (result != ESP_OK) return result;
    dhcps_offer_t offer = OFFER_DNS;
    return esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer, sizeof(offer));
}

static int openDnsSocket(void) {
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    struct ifreq interface = {0};
    if (!ap || esp_netif_get_ip_info(ap, &info) != ESP_OK ||
        esp_netif_get_netif_impl_name(ap, interface.ifr_name) != ESP_OK) return -1;
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    const struct timeval timeout = {.tv_sec = 1};
    const struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53),
        .sin_addr = {.s_addr = info.ip.addr}};
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void dnsTask(void *context) {
    int fd = (int)(intptr_t)context;
    uint8_t query[CAPTIVE_DNS_PACKET_MAX + 1], response[CAPTIVE_DNS_PACKET_MAX];
    esp_ip4_addr_t ip;
    if (esp_netif_str_to_ip4(CONFIG_BEER_AP_IP, &ip) != ESP_OK) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            fd = openDnsSocket();
            continue;
        }
        struct sockaddr_in peer = {0};
        socklen_t peerLength = sizeof(peer);
        const int length = recvfrom(fd, query, sizeof(query), 0,
            (struct sockaddr *)&peer, &peerLength);
        if (length < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(fd);
                fd = -1; // Nova tentativa sem afetar tarefas de controle/rede.
            }
            continue;
        }
        const size_t size = captive_dns_reply(query, (size_t)length,
            (const uint8_t *)&ip.addr, response, sizeof(response));
        if (size && peer.sin_family == AF_INET && peer.sin_port)
            (void)sendto(fd, response, size, 0, (struct sockaddr *)&peer, peerLength);
        // Cede CPU mesmo se um cliente mandar varias consultas consecutivas.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t captive_portal_start(void) {
    if (started) return ESP_ERR_INVALID_STATE;
    const int fd = openDnsSocket();
    if (fd < 0) return ESP_FAIL;
    if (xTaskCreate(dnsTask, "captive_dns", 4096, (void *)(intptr_t)fd, 1, NULL) != pdPASS) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "Portal local ativo: http://%s/; DNS somente no AP", CONFIG_BEER_AP_IP);
    return ESP_OK;
}

#else
esp_err_t captive_portal_configure_dhcp(esp_netif_t *ap) { (void)ap; return ESP_OK; }
esp_err_t captive_portal_start(void) { return ESP_OK; }
#endif
