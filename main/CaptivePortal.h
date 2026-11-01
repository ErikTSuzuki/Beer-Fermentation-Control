#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_err.h"
#include "esp_netif.h"

// Somente o AP; chamar com DHCP parado e IP local ja definido.
esp_err_t captive_portal_configure_dhcp(esp_netif_t *ap);
// Apos iniciar HTTP. Falhas sao opcionais: o endereco IP direto continua valido.
esp_err_t captive_portal_start(void);

#endif
