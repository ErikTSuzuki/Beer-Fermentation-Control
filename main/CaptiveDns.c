#include "CaptiveDns.h"

#include <stdbool.h>
#include <string.h>

static uint16_t read16(const uint8_t *bytes) {
    return ((uint16_t)bytes[0] << 8) | bytes[1];
}

size_t captive_dns_reply(const uint8_t *query, size_t length, const uint8_t ipv4[4],
                         uint8_t *response, size_t capacity) {
    if (!query || !ipv4 || !response || length < 17 || length > CAPTIVE_DNS_PACKET_MAX ||
        (read16(query + 2) & 0xfa00) || read16(query + 4) != 1 ||
        read16(query + 6) || read16(query + 8)) return 0;

    // Uma unica pergunta nao comprimida. Nomes comprimidos/loops sao rejeitados.
    size_t end = 12;
    for (;;) {
        if (end >= length) return 0;
        const uint8_t labelLength = query[end++];
        if (labelLength > 63 || labelLength > length - end) return 0;
        end += labelLength;
        if (end - 12 > 255) return 0;
        if (!labelLength) break;
    }
    if (length - end < 4) return 0;
    const bool answer = read16(query + end) == 1 && read16(query + end + 2) == 1;
    end += 4;
    const size_t needed = end + (answer ? 16 : 0);
    if (capacity < needed) return 0;

    memcpy(response, query, end);
    response[2] = 0x84 | (query[2] & 1); // Resposta autoritativa; preserva RD.
    response[3] = 0; // Sem recursao, erro, AD ou CD.
    memset(response + 6, 0, 6); // Nao copia adicionais EDNS da pergunta.
    if (answer) {
        response[7] = 1;
        static const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
        memcpy(response + end, record, sizeof(record));
        memcpy(response + end + sizeof(record), ipv4, 4);
    }
    // AAAA/HTTPS/SVCB e outros tipos recebem NOERROR sem respostas: permite
    // fallback IPv4. TTL zero evita reter o IP do portal ao trocar de rede.
    return needed;
}
