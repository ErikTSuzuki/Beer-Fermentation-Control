#pragma once

#include <cstdint>
#include "esp_err.h"

namespace mar2406_display {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;

// Uso exclusivo por uma tarefa. Tela ILI9341, barramento paralelo de 8 bits.
esp_err_t initialize();
void fill(uint16_t color);
void text(int x, int y, const char *value, uint16_t color,
          uint16_t background = 0x0000, int scale = 2);

} // namespace mar2406_display
