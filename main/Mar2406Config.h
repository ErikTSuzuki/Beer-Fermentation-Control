#pragma once

#include "driver/gpio.h"

namespace mar2406_config {

// Pinagem do LCD conforme a ligacao informada.
// LCD_RD deve ser ligado a 3V3 (somente escrita). GND deve ser comum.
// Preserva GPIO4 (DS18B20), UART e flash.
constexpr gpio_num_t DATA[8] = {
    GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_16, GPIO_NUM_17,
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22
};
constexpr gpio_num_t WR = GPIO_NUM_23;
constexpr gpio_num_t RS = GPIO_NUM_25;
constexpr gpio_num_t CS = GPIO_NUM_26;
constexpr gpio_num_t RST = GPIO_NUM_27;
constexpr bool TEST_ENABLED = true;

} // namespace mar2406_config
