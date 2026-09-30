#include "Mar2406Display.h"
#include "Mar2406Config.h"

#include <initializer_list>
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

using namespace mar2406_config;
bool ready = false;

// Escrita conservadora para teste; nao desabilita interrupcoes do ESP-NOW.
void writeByte(uint8_t value) {
    gpio_set_level(WR, 0);
    for (int bit = 0; bit < 8; ++bit) {
        gpio_set_level(DATA[bit], (value >> bit) & 1);
    }
    esp_rom_delay_us(1);
    gpio_set_level(WR, 1);
    esp_rom_delay_us(1);
}

void command(uint8_t value, std::initializer_list<uint8_t> data = {}) {
    gpio_set_level(CS, 0);
    gpio_set_level(RS, 0);
    writeByte(value);
    gpio_set_level(RS, 1);
    for (uint8_t byte : data) {
        writeByte(byte);
    }
    gpio_set_level(CS, 1);
}

void window(int x, int y, int width, int height) {
    const int right = x + width - 1;
    const int bottom = y + height - 1;
    command(0x2A, {uint8_t(x >> 8), uint8_t(x),
                   uint8_t(right >> 8), uint8_t(right)});
    command(0x2B, {uint8_t(y >> 8), uint8_t(y),
                   uint8_t(bottom >> 8), uint8_t(bottom)});
    command(0x2C);
    gpio_set_level(CS, 0);
}

void pixel(uint16_t color) {
    writeByte(color >> 8);
    writeByte(color & 0xFF);
}

// Fonte local 5x7: numeros, letras maiusculas e pontuacao da demonstracao.
constexpr uint8_t DIGITS[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}
};
constexpr uint8_t LETTERS[26][5] = {
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

uint8_t column(char c, int col) {
    if (col >= 5) return 0;
    if (c >= '0' && c <= '9') return DIGITS[c - '0'][col];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (c >= 'A' && c <= 'Z') return LETTERS[c - 'A'][col];
    if (c == '.') return col == 2 ? 0x60 : 0;
    if (c == ':') return col == 2 ? 0x36 : 0;
    if (c == '-') return 0x08;
    return 0;
}

} // namespace

namespace mar2406_display {

esp_err_t initialize() {
    if (ready) return ESP_OK;
    uint64_t mask = 0;
    const gpio_num_t pins[] = {DATA[0], DATA[1], DATA[2], DATA[3],
        DATA[4], DATA[5], DATA[6], DATA[7], WR, RS, CS, RST};
    for (gpio_num_t pin : pins) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || pin == GPIO_NUM_4 ||
            (mask & (1ULL << pin))) {
            return ESP_ERR_INVALID_ARG;
        }
        mask |= 1ULL << pin;
    }
    gpio_config_t config = {};
    config.pin_bit_mask = mask;
    config.mode = GPIO_MODE_OUTPUT;
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) return result;

    gpio_set_level(CS, 1);
    gpio_set_level(WR, 1);
    gpio_set_level(RS, 1);
    gpio_set_level(RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x01); // Software reset.
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x28); // Display off durante configuracao.
    command(0xC0, {0x23});
    command(0xC1, {0x10});
    command(0xC5, {0x3E, 0x28});
    command(0xC7, {0x86});
    command(0x36, {0x28}); // Landscape, BGR, 320x240.
    command(0x3A, {0x55}); // RGB565, dois bytes por pixel.
    command(0xB1, {0x00, 0x18});
    command(0xB6, {0x08, 0x82, 0x27});
    command(0x11); // Sleep out.
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x29); // Display on.
    vTaskDelay(pdMS_TO_TICKS(20));
    ready = true;
    return ESP_OK;
}

void fill(uint16_t color) {
    if (!ready) return;
    // Libera CPU a cada linha para nao monopolizar a tarefa idle.
    for (int y = 0; y < HEIGHT; ++y) {
        window(0, y, WIDTH, 1);
        for (int x = 0; x < WIDTH; ++x) pixel(color);
        gpio_set_level(CS, 1);
        vTaskDelay(1);
    }
}

void text(int x, int y, const char *value, uint16_t color,
          uint16_t background, int scale) {
    if (!ready || !value || scale < 1 || scale > 4 || x < 0 || y < 0 ||
        y > HEIGHT - 8 * scale) return;
    while (*value && x <= WIDTH - 6 * scale) {
        window(x, y, 6 * scale, 8 * scale);
        for (int row = 0; row < 8 * scale; ++row) {
            for (int col = 0; col < 6 * scale; ++col) {
                const bool set = column(*value, col / scale) & (1 << (row / scale));
                pixel(set ? color : background);
            }
        }
        gpio_set_level(CS, 1);
        ++value;
        x += 6 * scale;
        vTaskDelay(1);
    }
}

} // namespace mar2406_display
