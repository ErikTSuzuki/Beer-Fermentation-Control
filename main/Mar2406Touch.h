#ifndef MAR2406_TOUCH_H
#define MAR2406_TOUCH_H

#include <stdbool.h>
#include "esp_err.h"

// Usa o mesmo barramento da TFT. Chamar apenas na tarefa que desenha a tela.
esp_err_t mar2406_touch_initialize(void);
bool mar2406_touch_read(int *x, int *y);

#endif
