#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include <stddef.h>
#include <stdint.h>

#define CAPTIVE_DNS_PACKET_MAX 512

// Resposta DNS local, sem rede/RTOS. IPv4 em ordem dos bytes da rede.
// Retorna zero para mensagens invalidas ou buffer insuficiente.
size_t captive_dns_reply(const uint8_t *query, size_t length, const uint8_t ipv4[4],
                         uint8_t *response, size_t capacity);

#endif
