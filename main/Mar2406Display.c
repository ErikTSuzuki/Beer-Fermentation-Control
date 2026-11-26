#include "Mar2406Display.h"
#include "Mar2406Config.h"

#include <stddef.h>
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

static bool ready = false;
static uint32_t dataMask;
static uint32_t dataLevels[256];

static void writeByte(uint8_t value) {
    const uint32_t writeMask = UINT32_C(1) << MAR2406_WR;
    const uint32_t levels = dataLevels[value];
    // Atualiza os oito bits juntos e preserva os demais GPIOs do ESP32.
    GPIO.out_w1tc = writeMask | (dataMask & ~levels);
    GPIO.out_w1ts = levels;
    esp_rom_delay_us(1);
    GPIO.out_w1ts = writeMask;
    esp_rom_delay_us(1);
}

static void command(uint8_t value, const uint8_t *data, size_t size) {
    gpio_set_level(MAR2406_CS, 0);
    gpio_set_level(MAR2406_RS, 0);
    writeByte(value);
    gpio_set_level(MAR2406_RS, 1);
    for (size_t i = 0; i < size; ++i) {
        writeByte(data[i]);
    }
    gpio_set_level(MAR2406_CS, 1);
}

static void window(int x, int y, int width, int height) {
    const int right = x + width - 1;
    const int bottom = y + height - 1;
    command(0x2A, (const uint8_t[]){(uint8_t)(x >> 8), (uint8_t)(x),
                   (uint8_t)(right >> 8), (uint8_t)(right)}, 4);
    command(0x2B, (const uint8_t[]){(uint8_t)(y >> 8), (uint8_t)(y),
                   (uint8_t)(bottom >> 8), (uint8_t)(bottom)}, 4);
    gpio_set_level(MAR2406_CS, 0);
    gpio_set_level(MAR2406_RS, 0);
    writeByte(0x2C);
    gpio_set_level(MAR2406_RS, 1);
}

esp_err_t mar2406_display_initialize(void) {
    if (ready) return ESP_OK;
    uint64_t mask = 0;
    const gpio_num_t pins[] = {MAR2406_DATA[0], MAR2406_DATA[1], MAR2406_DATA[2], MAR2406_DATA[3],
        MAR2406_DATA[4], MAR2406_DATA[5], MAR2406_DATA[6], MAR2406_DATA[7], MAR2406_WR, MAR2406_RS, MAR2406_CS, MAR2406_RST};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        const gpio_num_t pin = pins[i];
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (mask & (1ULL << pin))) {
            return ESP_ERR_INVALID_ARG;
        }
        mask |= 1ULL << pin;
    }
    for (unsigned bit = 0; bit < 8; ++bit) {
        if (MAR2406_DATA[bit] >= 32) return ESP_ERR_INVALID_ARG;
        const uint32_t pinMask = UINT32_C(1) << MAR2406_DATA[bit];
        dataMask |= pinMask;
        for (unsigned value = 0; value < 256; ++value) {
            if (value & (1u << bit)) dataLevels[value] |= pinMask;
        }
    }
    gpio_config_t config = {0};
    config.pin_bit_mask = mask;
    config.mode = GPIO_MODE_OUTPUT;
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) return result;

    gpio_set_level(MAR2406_CS, 1);
    gpio_set_level(MAR2406_WR, 1);
    gpio_set_level(MAR2406_RS, 1);
    gpio_set_level(MAR2406_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MAR2406_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x01, NULL, 0); // Software reset.
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x28, NULL, 0); // Display off durante configuracao.
    command(0xC0, (const uint8_t[]){0x23}, 1);
    command(0xC1, (const uint8_t[]){0x10}, 1);
    command(0xC5, (const uint8_t[]){0x3E, 0x28}, 2);
    command(0xC7, (const uint8_t[]){0x86}, 1);
    command(0x36, (const uint8_t[]){0x28}, 1); // Landscape, BGR, 320x240.
    command(0x3A, (const uint8_t[]){0x55}, 1); // RGB565, dois bytes por pixel.
    command(0xB1, (const uint8_t[]){0x00, 0x18}, 2);
    command(0xB6, (const uint8_t[]){0x08, 0x82, 0x27}, 3);
    command(0x11, NULL, 0); // Sleep out.
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x29, NULL, 0); // Display on.
    vTaskDelay(pdMS_TO_TICKS(20));
    GPIO.out_w1ts = dataMask;
    ready = true;
    return ESP_OK;
}

void mar2406_display_draw(int x, int y, int width, int height,
                          const uint8_t *pixels, size_t stride) {
    if (!ready) return;

    window(x, y, width, height);
    for (int row = 0; row < height; ++row) {
        const uint8_t *pixel = pixels + row * stride;
        for (int col = 0; col < width; ++col, pixel += 2) {
            // O ILI9341 recebe primeiro o byte mais significativo do RGB565.
            writeByte(pixel[1]);
            writeByte(pixel[0]);
        }
    }
    gpio_set_level(MAR2406_CS, 1);
    // Em repouso, coloca as linhas do touch no mesmo nivel.
    GPIO.out_w1ts = dataMask;
}
