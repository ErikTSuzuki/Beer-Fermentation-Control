#pragma once

#include "driver/gpio.h"

namespace relay_config {

// Livres nesta pinagem, sem conflito com LCD, RTC ou DS18B20.
constexpr gpio_num_t HEATER = GPIO_NUM_32;
constexpr gpio_num_t COOLER = GPIO_NUM_33;
// Confirme o nivel de acionamento do modulo. HIGH: altere para false.
constexpr bool ACTIVE_LOW = true;
// Teste de bancada continuo: alterna os reles. Desative antes de usar cargas.
constexpr bool TEST_ENABLED = true;
constexpr unsigned START_DELAY_MS = 5000;
constexpr unsigned ON_TIME_MS = 5000;

} // namespace relay_config
