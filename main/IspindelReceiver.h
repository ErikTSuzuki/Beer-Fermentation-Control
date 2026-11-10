#ifndef ISPINDEL_RECEIVER_H
#define ISPINDEL_RECEIVER_H

#include "BrewProtocol.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

typedef struct {
    BrewTelemetry packet;
    TickType_t receivedAt;
    // Idade da densidade sem depender da volta do contador de ticks.
    int64_t receivedAtUs;
    // Uma tentativa invalida do sensor selecionado invalida o cache.
    bool receiveFailed;
} IspindelReading;

// Mesmo prazo para tela, interface, receita e nuvem; nao depende de ticks de 32 bits.
static inline bool ispindel_reading_is_current(const IspindelReading *reading, int64_t nowUs) {
    return reading && !reading->receiveFailed && reading->receivedAtUs >= 0 &&
        nowUs >= reading->receivedAtUs && nowUs - reading->receivedAtUs <
            (int64_t)CONFIG_BEER_ISPINDEL_STALE_SECONDS * 1000000;
}

// Inicializar uma vez; start requer SoftAP e caixa postal ja inicializados.
bool ispindel_receiver_create_queue(void);
void ispindel_receiver_start(void);
// Copia segura; false se nao ha leitura ou a ultima tentativa falhou.
// O chamador ainda deve verificar a idade da amostra.
bool ispindel_receiver_latest(IspindelReading *reading);

#endif
