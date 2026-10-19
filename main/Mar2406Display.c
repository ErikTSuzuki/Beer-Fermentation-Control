#include "Mar2406Display.h"
#include "Mar2406Config.h"

#include <stddef.h>
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool ready = false;

// Escrita conservadora para teste; nao desabilita interrupcoes do ESP-NOW.
static void writeByte(uint8_t value) {
    gpio_set_level(MAR2406_WR, 0);
    for (int bit = 0; bit < 8; ++bit) {
        gpio_set_level(MAR2406_DATA[bit], (value >> bit) & 1);
    }
    esp_rom_delay_us(1);
    gpio_set_level(MAR2406_WR, 1);
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
    command(0x2C, NULL, 0);
    gpio_set_level(MAR2406_CS, 0);
}

static void pixel(uint16_t color) {
    writeByte(color >> 8);
    writeByte(color & 0xFF);
}

// Fonte local 5x7: numeros, letras maiusculas e pontuacao da demonstracao.
static const uint8_t DIGITS[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};
static const uint8_t LETTERS[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0,0x41,0x7F,0x41,0}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
};

static uint8_t column(char c, int col) {
    if (col >= 5) return 0;
    if (c >= '0' && c <= '9') return DIGITS[c - '0'][col];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'][col];
    if (c == '.') return col == 2 ? 0x60 : 0;
    if (c == ':') return col == 2 ? 0x36 : 0;
    if (c == '-') return 0x08;
    return 0;
}

esp_err_t mar2406_display_initialize(void) {
    if (ready) return ESP_OK;
    uint64_t mask = 0;
    const gpio_num_t pins[] = {MAR2406_DATA[0], MAR2406_DATA[1], MAR2406_DATA[2], MAR2406_DATA[3],
        MAR2406_DATA[4], MAR2406_DATA[5], MAR2406_DATA[6], MAR2406_DATA[7], MAR2406_WR, MAR2406_RS, MAR2406_CS, MAR2406_RST};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        const gpio_num_t pin = pins[i];
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || pin == GPIO_NUM_4 ||
            (mask & (1ULL << pin))) {
            return ESP_ERR_INVALID_ARG;
        }
        mask |= 1ULL << pin;
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
    ready = true;
    return ESP_OK;
}

void mar2406_display_fill(uint16_t color) {
    if (!ready) return;
    // Libera CPU a cada linha para nao monopolizar a tarefa idle.
    for (int y = 0; y < MAR2406_HEIGHT; ++y) {
        window(0, y, MAR2406_WIDTH, 1);
        for (int x = 0; x < MAR2406_WIDTH; ++x) pixel(color);
        gpio_set_level(MAR2406_CS, 1);
        vTaskDelay(1);
    }
}

void mar2406_display_text(int x, int y, const char *value, uint16_t color,
          uint16_t background, int scale) {
    if (!ready || !value || scale < 1 || scale > 4 || x < 0 || y < 0 ||
        y > MAR2406_HEIGHT - 8 * scale) return;
    while (*value && x <= MAR2406_WIDTH - 6 * scale) {
        window(x, y, 6 * scale, 8 * scale);
        for (int row = 0; row < 8 * scale; ++row) {
            for (int col = 0; col < 6 * scale; ++col) {
                const bool set = column(*value, col / scale) & (1 << (row / scale));
                pixel(set ? color : background);
            }
        }
        gpio_set_level(MAR2406_CS, 1);
        ++value;
        x += 6 * scale;
        vTaskDelay(1);
    }
}
