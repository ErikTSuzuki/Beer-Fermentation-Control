#ifndef RELAY_CONFIG_H
#define RELAY_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Livres nesta pinagem, sem conflito com LCD, RTC ou DS18B20.
#define RELAY_HEATER GPIO_NUM_32
#define RELAY_COOLER GPIO_NUM_33
// Confirme o nivel de acionamento do modulo. HIGH: altere para false.
#define RELAY_ACTIVE_LOW true
// Os GPIOs pertencem exclusivamente a tarefa TemperatureControl.

#endif
