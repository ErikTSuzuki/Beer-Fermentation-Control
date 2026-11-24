#ifndef WIFI_STATION_H
#define WIFI_STATION_H

#include <stdbool.h>
#include "esp_err.h"

// Inicializa AP+STA: rede local do iSpindel e conexao externa opcional.
void wifi_station_initialize(void);
// True somente depois de obter IP na rede configurada.
bool wifi_station_is_connected(void);
bool wifi_station_can_configure(void);
bool wifi_station_supports_enterprise(void);
// Usuario vazio seleciona uma rede aberta ou com senha compartilhada.
esp_err_t wifi_station_set_uplink(const char *ssid, const char *password,
                                 const char *username, const char *identity);
// Adia a troca de rede para dar tempo de publicar a confirmacao pela VM.
esp_err_t wifi_station_set_uplink_remote(const char *ssid, const char *password,
                                        const char *username, const char *identity);

#endif
