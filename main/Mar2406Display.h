#ifndef MAR2406_DISPLAY_H
#define MAR2406_DISPLAY_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MAR2406_WIDTH 320
#define MAR2406_HEIGHT 240

// Uso exclusivo por uma tarefa. Tela ILI9341, barramento paralelo de 8 bits.
esp_err_t mar2406_display_initialize(void);
// Envia uma area RGB565 little-endian. O stride da origem e dado em bytes.
void mar2406_display_draw(int x, int y, int width, int height,
                          const uint8_t *pixels, size_t stride);

#endif
