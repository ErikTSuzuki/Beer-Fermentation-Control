#ifndef LOCAL_TEMPERATURE_H
#define LOCAL_TEMPERATURE_H

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

typedef struct {
    float temperatureC;
    TickType_t sampledAt;
    bool valid;
} LocalTemperature;

// Inicializar uma vez, antes de iniciar as tarefas.
bool local_temperature_create_queue(void);
void local_temperature_start(void);
// Copia a ultima amostra sem esperar; false enquanto nao houver amostra.
bool local_temperature_latest(LocalTemperature *sample);

#endif
