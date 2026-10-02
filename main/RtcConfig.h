#pragma once

#include "driver/gpio.h"

namespace rtc_config {

// GPIO21/22 ja pertencem a TFT. GPIO32/33 ficam livres para futuros reles.
// GPIO5/15 sao strapping: SDA e SCL precisam de pull-ups para 3V3.
constexpr gpio_num_t SDA = GPIO_NUM_5;
constexpr gpio_num_t SCL = GPIO_NUM_15;
// Somente se o RTC perdeu a hora (OSF) ou possui calendario invalido.
// Hora aproximada, no fuso do computador que compila; nunca reajusta RTC valido.
constexpr bool INITIALIZE_FROM_BUILD_TIME = true;

} // namespace rtc_config
