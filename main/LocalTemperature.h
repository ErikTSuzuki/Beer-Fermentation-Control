#pragma once

#include "freertos/FreeRTOS.h"

struct LocalTemperature {
    float temperatureC;
    TickType_t sampledAt;
    bool valid;
};

namespace local_temperature {

// Inicializar uma vez, antes de iniciar as tarefas.
bool createQueue();
void start();
// Copia a ultima amostra sem esperar; false enquanto nao houver amostra.
bool latest(LocalTemperature &sample);

} // namespace local_temperature
