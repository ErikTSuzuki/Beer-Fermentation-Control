#ifndef RELAY_CONFIG_H
#define RELAY_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Livres nesta pinagem, sem conflito com LCD, RTC ou DS18B20.
#define RELAY_HEATER GPIO_NUM_32
#define RELAY_COOLER GPIO_NUM_33
// Confirme o nivel de acionamento do modulo. HIGH: altere para false.
#define RELAY_ACTIVE_LOW true
// Teste de bancada continuo: alterna os reles. Desative antes de usar cargas.
#define RELAY_TEST_ENABLED true
#define RELAY_START_DELAY_MS 5000U
#define RELAY_ON_TIME_MS 5000U

#endif
