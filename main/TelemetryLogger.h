#ifndef TELEMETRY_LOGGER_H
#define TELEMETRY_LOGGER_H

#include "BrewProtocol.h"

void telemetry_logger_print_reading(const BrewTelemetry *packet,
                                    const uint8_t *mac, int rssi, uint32_t count);

#endif
