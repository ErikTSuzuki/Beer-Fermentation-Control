#ifndef MAR2406_DISPLAY_H
#define MAR2406_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

#define MAR2406_WIDTH 320
#define MAR2406_HEIGHT 240

// Uso exclusivo por uma tarefa. Tela ILI9341, barramento paralelo de 8 bits.
esp_err_t mar2406_display_initialize(void);
void mar2406_display_fill(uint16_t color);
void mar2406_display_text(int x, int y, const char *value, uint16_t color,
                          uint16_t background, int scale);

#endif
