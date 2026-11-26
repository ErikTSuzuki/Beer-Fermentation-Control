#ifndef MAR2406_CONFIG_H
#define MAR2406_CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Ordem logica dos bits LCD_D0 a LCD_D7.
// LCD_RD fica em 3V3 (somente escrita), com GND comum.
#define MAR2406_D0 GPIO_NUM_22
#define MAR2406_D1 GPIO_NUM_23
static const gpio_num_t MAR2406_DATA[8] = {
    MAR2406_D0, MAR2406_D1, GPIO_NUM_4, GPIO_NUM_16,
    GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_21
};
#define MAR2406_WR GPIO_NUM_14
#define MAR2406_RS GPIO_NUM_32
#define MAR2406_CS GPIO_NUM_33
#define MAR2406_RST GPIO_NUM_27
#define MAR2406_TEST_ENABLED true

// Ligacao do touch resistivo no exemplo LCDWIKI para este shield.
#define MAR2406_TOUCH_XP MAR2406_D0
#define MAR2406_TOUCH_YM MAR2406_D1
#define MAR2406_TOUCH_XM MAR2406_RS
#define MAR2406_TOUCH_YP MAR2406_CS

// Pontos do touch em retrato, em escala de 0 a 1000. O driver gira para paisagem.
#define MAR2406_TOUCH_LEFT   121
#define MAR2406_TOUCH_RIGHT  886
#define MAR2406_TOUCH_TOP     81
#define MAR2406_TOUCH_BOTTOM 873

#endif
