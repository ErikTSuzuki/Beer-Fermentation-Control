#pragma once

#include "BrewProtocol.h"

namespace telemetry_logger {

void printReading(const BrewTelemetry &p, const uint8_t *mac, int rssi, uint32_t count);

} // namespace telemetry_logger
