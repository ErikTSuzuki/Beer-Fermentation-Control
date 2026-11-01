#ifndef WIFI_STATION_H
#define WIFI_STATION_H

#include <stdbool.h>

// Inicializa AP+STA: rede local do iSpindel e conexao externa opcional.
void wifi_station_initialize(void);
// True somente depois de obter IP na rede configurada.
bool wifi_station_is_connected(void);

#endif
