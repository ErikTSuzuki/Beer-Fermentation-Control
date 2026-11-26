#include "Mar2406Touch.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"

#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"

#define PIN_MASK(pin) (UINT64_C(1) << (pin))
#define TOUCH_PINS (PIN_MASK(MAR2406_TOUCH_XP) | PIN_MASK(MAR2406_TOUCH_XM) | \
                    PIN_MASK(MAR2406_TOUCH_YP) | PIN_MASK(MAR2406_TOUCH_YM))

static adc_oneshot_unit_handle_t adc;
static adc_channel_t xmChannel;
static adc_channel_t ypChannel;

static void drive(gpio_num_t pin, int level) {
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

static void releaseTouchPins(void) {
    const gpio_config_t config = {.pin_bit_mask = TOUCH_PINS, .mode = GPIO_MODE_DISABLE};
    gpio_config(&config);
}

static void restoreDisplayPins(void) {
    releaseTouchPins();
    drive(MAR2406_TOUCH_YP, 1);
    drive(MAR2406_TOUCH_XM, 1);
    drive(MAR2406_TOUCH_XP, 1);
    drive(MAR2406_TOUCH_YM, 1);
}

esp_err_t mar2406_touch_initialize(void) {
    adc_unit_t xmUnit, ypUnit;
    if (adc_oneshot_io_to_channel(MAR2406_TOUCH_XM, &xmUnit, &xmChannel) != ESP_OK ||
        adc_oneshot_io_to_channel(MAR2406_TOUCH_YP, &ypUnit, &ypChannel) != ESP_OK ||
        xmUnit != ADC_UNIT_1 || ypUnit != ADC_UNIT_1) return ESP_ERR_NOT_SUPPORTED;

    // O ADC1 pode ser usado enquanto o radio Wi-Fi esta ativo.
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t result = adc_oneshot_new_unit(&unit, &adc);
    if (result != ESP_OK) return result;

    const adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_12};
    result = adc_oneshot_config_channel(adc, xmChannel, &channel);
    if (result == ESP_OK) result = adc_oneshot_config_channel(adc, ypChannel, &channel);
    restoreDisplayPins();
    if (result != ESP_OK) {
        adc_oneshot_del_unit(adc);
        adc = NULL;
    }
    return result;
}

static bool contactDetected(void) {
    releaseTouchPins();
    drive(MAR2406_TOUCH_YP, 1);
    drive(MAR2406_TOUCH_YM, 1);
    gpio_set_direction(MAR2406_TOUCH_XM, GPIO_MODE_INPUT);
    gpio_pulldown_en(MAR2406_TOUCH_XM);
    // O toque une as duas camadas e eleva XM, que em repouso fica em LOW.
    esp_rom_delay_us(40);
    return gpio_get_level(MAR2406_TOUCH_XM) != 0;
}

static bool readVoltage(adc_channel_t channel, int *value) {
    esp_rom_delay_us(40);
    int samples[3];
    for (unsigned i = 0; i < 3; ++i) {
        if (adc_oneshot_read(adc, channel, &samples[i]) != ESP_OK) return false;
    }
    const int lower = samples[0] < samples[1] ? samples[0] : samples[1];
    const int upper = samples[0] > samples[1] ? samples[0] : samples[1];
    *value = samples[2];
    if (*value < lower) *value = lower;
    if (*value > upper) *value = upper;
    return true;
}

static bool readAxis(gpio_num_t plus, gpio_num_t minus, gpio_num_t sense,
                     adc_channel_t channel, int *position) {
    releaseTouchPins();
    rtc_gpio_init(sense);
    drive(plus, 1);
    drive(minus, 0);
    int forward, reverse;
    if (!readVoltage(channel, &forward)) return false;

    // A segunda leitura com polaridade invertida reduz o erro nas bordas.
    gpio_set_level(plus, 0);
    gpio_set_level(minus, 1);
    if (!readVoltage(channel, &reverse) || forward + reverse < 1500) return false;
    *position = 1000 * reverse / (forward + reverse);
    return true;
}

static int mapCoordinate(int value, int start, int end, int length) {
    const int pixel = (value - start) * (length - 1) / (end - start);
    if (pixel < 0) return 0;
    if (pixel >= length) return length - 1;
    return pixel;
}

bool mar2406_touch_read(int *x, int *y) {
    if (!adc) return false;

    // WR fica em HIGH durante toda a amostragem, inclusive quando CS muda.
    gpio_set_level(MAR2406_WR, 1);
    int portraitX, portraitY;
    const bool pressed = contactDetected() &&
        readAxis(MAR2406_TOUCH_XP, MAR2406_TOUCH_XM, MAR2406_TOUCH_YP, ypChannel, &portraitX) &&
        readAxis(MAR2406_TOUCH_YP, MAR2406_TOUCH_YM, MAR2406_TOUCH_XM, xmChannel, &portraitY) &&
        contactDetected();
    restoreDisplayPins();
    if (!pressed) return false;

    *x = mapCoordinate(portraitY, MAR2406_TOUCH_TOP, MAR2406_TOUCH_BOTTOM, MAR2406_WIDTH);
    *y = mapCoordinate(portraitX, MAR2406_TOUCH_RIGHT, MAR2406_TOUCH_LEFT, MAR2406_HEIGHT);
    return true;
}
