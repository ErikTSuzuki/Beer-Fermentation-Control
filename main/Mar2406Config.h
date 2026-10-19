#ifndef MAR2406_CONFIG_H
#define MAR2406_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Pinagem do LCD conforme a ligacao informada.
// LCD_RD deve ser ligado a 3V3 (somente escrita). GND deve ser comum.
// Preserva GPIO4 (DS18B20), UART e flash.
static const gpio_num_t MAR2406_DATA[8] = {
    GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_16, GPIO_NUM_17,
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21, GPIO_NUM_22
};
#define MAR2406_WR GPIO_NUM_23
#define MAR2406_RS GPIO_NUM_25
#define MAR2406_CS GPIO_NUM_26
#define MAR2406_RST GPIO_NUM_27
#define MAR2406_TEST_ENABLED true

#endif
