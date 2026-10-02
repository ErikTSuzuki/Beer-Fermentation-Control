#pragma once

#include "BrewProtocol.h"
#include "freertos/FreeRTOS.h"

namespace ispindel_receiver {

constexpr uint32_t STALE_AFTER_MS = 35000;

struct Reading {
    BrewTelemetry packet;
    TickType_t receivedAt;
};

// Inicializar uma vez; start requer Wi-Fi e filas ja inicializados.
bool createQueue();
void start();
// Copia segura da ultima leitura validada, sem consumir a fila de recepcao.
bool latest(Reading &reading);

} // namespace ispindel_receiver
