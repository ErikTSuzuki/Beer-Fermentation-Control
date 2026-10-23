#ifndef BREW_PROTOCOL_H
#define BREW_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#define BREW_MAGIC UINT32_C(0x42525731)
#define BREW_VERSION ((uint8_t)1)
// Canal inicial do SoftAP; AP+STA acompanha o canal da rede externa.
#define BREW_CHANNEL ((uint8_t)6)
#define BREW_HTTP_PORT UINT16_C(80)
#define BREW_HTTP_PATH "/ispindel"
#define BREW_HTTP_BODY_MAX 512

typedef enum {
    BREW_TELEMETRY = 1,
    BREW_ACK = 2
} BrewType;

typedef enum {
    BREW_UNIT_UNKNOWN = 0,
    BREW_UNIT_SG = 1,
    BREW_UNIT_PLATO = 2
} BrewGravityUnit;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t gravityUnit;
    uint8_t reserved;

    uint32_t sensorId;
    uint32_t bootId;
    uint32_t sequence;
} BrewHeader;

typedef struct {
    BrewHeader header;
    float temperatureC;
    float tiltDegrees;
    float gravity;
    float batteryV;
} BrewTelemetry;

typedef BrewHeader BrewAck;

_Static_assert(sizeof(float) == 4, "float incompatível");
_Static_assert(sizeof(BrewHeader) == 20, "Header incompatível");
_Static_assert(sizeof(BrewTelemetry) == 36, "Pacote incompatível");

static inline bool brew_header_valid(const BrewHeader *h, uint8_t type) {
    return h->magic == BREW_MAGIC &&
           h->version == BREW_VERSION &&
           h->type == type &&
           h->reserved == 0 &&
           h->gravityUnit <= BREW_UNIT_PLATO;
}

#endif
