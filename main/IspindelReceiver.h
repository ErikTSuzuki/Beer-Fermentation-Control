#ifndef ISPINDEL_RECEIVER_H
#define ISPINDEL_RECEIVER_H

#include "BrewProtocol.h"
#include "freertos/FreeRTOS.h"

#define ISPINDEL_STALE_AFTER_MS UINT32_C(35000)

typedef struct {
    BrewTelemetry packet;
    TickType_t receivedAt;
} IspindelReading;

// Inicializar uma vez; start requer Wi-Fi e filas ja inicializados.
bool ispindel_receiver_create_queue(void);
void ispindel_receiver_start(void);
// Copia segura da ultima leitura validada, sem consumir a fila de recepcao.
bool ispindel_receiver_latest(IspindelReading *reading);

#endif
