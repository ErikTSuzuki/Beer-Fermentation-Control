#ifndef ISPINDEL_RECEIVER_H
#define ISPINDEL_RECEIVER_H

#include "BrewProtocol.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

// Conversao direta evita overflow de pdMS_TO_TICKS no FreeRTOS 32 bits.
#define ISPINDEL_STALE_AFTER_TICKS ((TickType_t)(\
    (uint64_t)CONFIG_BEER_ISPINDEL_STALE_SECONDS * configTICK_RATE_HZ))

typedef struct {
    BrewTelemetry packet;
    TickType_t receivedAt;
    // Idade da densidade sem depender da volta do contador de ticks.
    int64_t receivedAtUs;
} IspindelReading;

// Inicializar uma vez; start requer SoftAP e caixa postal ja inicializados.
bool ispindel_receiver_create_queue(void);
void ispindel_receiver_start(void);
// Copia segura da ultima leitura validada, sem consumir a fila de recepcao.
bool ispindel_receiver_latest(IspindelReading *reading);

#endif
