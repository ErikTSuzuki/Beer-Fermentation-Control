#ifndef RELAY_CONFIG_H
#define RELAY_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// J4 (aquecimento) e J5 (refrigeracao).
#define RELAY_HEATER GPIO_NUM_26
#define RELAY_COOLER GPIO_NUM_25
// Confirme o nivel de acionamento do modulo. HIGH: altere para false.
#define RELAY_ACTIVE_LOW false
// Os GPIOs pertencem exclusivamente a tarefa TemperatureControl.

#endif
