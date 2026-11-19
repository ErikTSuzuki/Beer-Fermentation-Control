#ifndef MAR2406_CONFIG_H
#define MAR2406_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Ordem logica dos bits LCD_D0 a LCD_D7.
// LCD_RD fica em 3V3 (somente escrita), com GND comum.
static const gpio_num_t MAR2406_DATA[8] = {
    GPIO_NUM_22, GPIO_NUM_23, GPIO_NUM_4, GPIO_NUM_16,
    GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21
};
#define MAR2406_WR GPIO_NUM_14
#define MAR2406_RS GPIO_NUM_32
#define MAR2406_CS GPIO_NUM_33
#define MAR2406_RST GPIO_NUM_27
#define MAR2406_TEST_ENABLED true

#endif
