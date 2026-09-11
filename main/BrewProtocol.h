#pragma once
#include <stdint.h>

constexpr uint32_t BREW_MAGIC = 0x42525731;
constexpr uint8_t BREW_VERSION = 1;
constexpr uint8_t BREW_CHANNEL = 6;

enum BrewType : uint8_t {
    BREW_TELEMETRY = 1,
    BREW_ACK = 2
};

enum BrewGravityUnit : uint8_t {
    BREW_UNIT_UNKNOWN = 0,
    BREW_UNIT_SG = 1,
    BREW_UNIT_PLATO = 2
};

struct BrewHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t gravityUnit;
    uint8_t reserved;

    uint32_t sensorId;
    uint32_t bootId;
    uint32_t sequence;
};

struct BrewTelemetry {
    BrewHeader header;
    float temperatureC;
    float tiltDegrees;
    float gravity;
    float batteryV;
};

using BrewAck = BrewHeader;

static_assert(sizeof(float) == 4, "float incompatível");
static_assert(sizeof(BrewHeader) == 20, "Header incompatível");
static_assert(sizeof(BrewTelemetry) == 36, "Pacote incompatível");

inline bool brewHeaderValid(const BrewHeader &h, uint8_t type) {
    return h.magic == BREW_MAGIC &&
           h.version == BREW_VERSION &&
           h.type == type &&
           h.reserved == 0 &&
           h.gravityUnit <= BREW_UNIT_PLATO;
}