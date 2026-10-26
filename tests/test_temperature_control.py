"""Compile and exercise the real controller logic and RTOS/GPIO adapter on host.

Run: python tests/test_temperature_control.py [path-to-gcc]
Artifacts stay in build/control-tests. No board or real relays are accessed.
"""
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/control-tests"

LOGIC_TEST = r'''
#include "TemperatureControlLogic.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define SEC(x) ((int64_t)(x) * 1000000)
#define CHECK(x) do { assert(x); ++checks; } while (0)
static unsigned checks;
static const TemperatureControlSettings settings = {-5, 40, .5f, .5f, SEC(300), SEC(30)};
static TemperatureControlCommand command = {18, true};
static TemperatureControlLogic logic;
static TemperatureControlDecision step(float temp, int64_t at, bool valid) {
    return temperature_control_logic_step(&logic, &settings, command, temp, valid, at);
}
int main(void) {
    CHECK(temperature_control_settings_valid(&settings, 18));
    CHECK(!temperature_control_settings_valid(NULL, 18));
    CHECK(!temperature_control_settings_valid(&settings, NAN));
    CHECK(!temperature_control_settings_valid(&settings, INFINITY));
    CHECK(!temperature_control_settings_valid(&settings, -5));
    CHECK(!temperature_control_settings_valid(&settings, 40));
    TemperatureControlSettings invalid = settings;
    invalid.heatingDifferentialC = 0;
    CHECK(!temperature_control_settings_valid(&invalid, 18));
    invalid = settings; invalid.coolingDifferentialC = NAN;
    CHECK(!temperature_control_settings_valid(&invalid, 18));
    invalid = settings; invalid.compressorOffUs = SEC(59);
    CHECK(!temperature_control_settings_valid(&invalid, 18));
    invalid = settings; invalid.changeoverUs = 0;
    CHECK(!temperature_control_settings_valid(&invalid, 18));

    temperature_control_logic_initialize(&logic, 0);
    CHECK(step(18, 0, true).state == CONTROL_IDLE);
    CHECK(step(18.49f, 0, true).state == CONTROL_IDLE);
    CHECK(step(17.51f, 0, true).state == CONTROL_IDLE);
    CHECK(step(18.5f, 0, true).state == CONTROL_WAIT_COMPRESSOR);
    CHECK(step(19, SEC(299), true).waitUs == SEC(1));
    CHECK(step(18.4f, SEC(300), true).state == CONTROL_IDLE); // demand disappeared
    CHECK(step(18.5f, SEC(300), true).state == CONTROL_COOLING);
    CHECK(step(18.2f, SEC(301), true).state == CONTROL_COOLING);
    CHECK(step(18, SEC(302), true).state == CONTROL_IDLE);
    CHECK(step(18.5f, SEC(303), true).waitUs == SEC(299));
    CHECK(step(19, SEC(601), true).state == CONTROL_WAIT_COMPRESSOR);
    CHECK(step(19, SEC(602), true).state == CONTROL_COOLING);

    // Fault stops immediately, starts a new full off timer and cannot bypass it.
    CHECK(step(19, SEC(603), false).state == CONTROL_SENSOR_FAULT);
    CHECK(logic.output == CONTROL_OUTPUT_OFF);
    CHECK(step(19, SEC(604), true).waitUs == SEC(299));
    command.enabled = false;
    CHECK(step(19, SEC(605), true).state == CONTROL_DISABLED);
    command.enabled = true;
    CHECK(step(19, SEC(606), true).waitUs == SEC(297));
    CHECK(step(19, SEC(903), true).state == CONTROL_COOLING);
    CHECK(step(NAN, SEC(904), true).state == CONTROL_SENSOR_FAULT);
    CHECK(step(INFINITY, SEC(905), true).state == CONTROL_SENSOR_FAULT);
    CHECK(step(-6, SEC(906), true).state == CONTROL_SENSOR_FAULT);
    CHECK(step(41, SEC(907), true).state == CONTROL_SENSOR_FAULT);

    temperature_control_logic_initialize(&logic, SEC(1000));
    CHECK(step(17.5f, SEC(1000), true).state == CONTROL_HEATING);
    CHECK(step(17.8f, SEC(1001), true).state == CONTROL_HEATING);
    CHECK(step(18, SEC(1002), true).state == CONTROL_IDLE);
    CHECK(step(18.5f, SEC(1300), true).state == CONTROL_COOLING);
    // Changing the target turns the active output off before requesting opposite.
    command.targetC = 22;
    CHECK(step(19, SEC(1301), true).state == CONTROL_IDLE);
    CHECK(step(19, SEC(1302), true).state == CONTROL_WAIT_CHANGEOVER);
    CHECK(step(19, SEC(1330), true).waitUs == SEC(1));
    CHECK(step(19, SEC(1331), true).state == CONTROL_HEATING);
    command.targetC = 18;
    CHECK(step(19, SEC(1332), true).state == CONTROL_IDLE);
    CHECK(step(19, SEC(1333), true).state == CONTROL_WAIT_COMPRESSOR);
    CHECK(step(19, SEC(1600), true).waitUs == SEC(1));
    CHECK(step(19, SEC(1601), true).state == CONTROL_COOLING);
    command.targetC = NAN;
    CHECK(step(19, SEC(1602), true).state == CONTROL_CONFIG_FAULT);
    CHECK(logic.output == CONTROL_OUTPUT_OFF);
    command.targetC = 18;
    CHECK(step(19, SEC(1603), true).waitUs == SEC(299));

    // Long fermentation: timestamps beyond any 32-bit millisecond/tick counter.
    const int64_t month = SEC(INT64_C(90) * 86400);
    temperature_control_logic_initialize(&logic, month);
    CHECK(step(19, month + SEC(299), true).state == CONTROL_WAIT_COMPRESSOR);
    CHECK(step(19, month + SEC(300), true).state == CONTROL_COOLING);
    CHECK(step(18, month + SEC(301), true).state == CONTROL_IDLE);
    CHECK(step(19, month + SEC(600), true).waitUs == SEC(1));
    CHECK(step(19, month + SEC(601), true).state == CONTROL_COOLING);

    // Deterministic varied inputs: verify invariants independently of state labels.
    temperature_control_logic_initialize(&logic, 0);
    TemperatureControlOutput previous = CONTROL_OUTPUT_OFF, lastActive = CONTROL_OUTPUT_OFF;
    int64_t now = 0, coolerStopped = 0, outputStopped = 0;
    uint32_t random = 0x12345678;
    for (unsigned i = 0; i < 30000; ++i) {
        random = random * 1664525u + 1013904223u;
        now += SEC(1 + (random % 20));
        const float temperature = 10 + (float)((random >> 8) % 200) / 10;
        command.targetC = 14 + (float)((random >> 16) % 100) / 10;
        command.enabled = (random & 7) != 0;
        const bool valid = (random & 15) != 0;
        const TemperatureControlDecision decision = step(temperature, now, valid);
        const TemperatureControlOutput output = decision.output;
        CHECK(output == logic.output);
        CHECK(command.enabled && valid ? true : output == CONTROL_OUTPUT_OFF);
        if (previous != CONTROL_OUTPUT_OFF && output != previous) {
            CHECK(output == CONTROL_OUTPUT_OFF);
            outputStopped = now;
            lastActive = previous;
            if (previous == CONTROL_OUTPUT_COOLER) coolerStopped = now;
        }
        if (previous == CONTROL_OUTPUT_OFF && output != CONTROL_OUTPUT_OFF) {
            if (output == CONTROL_OUTPUT_COOLER) CHECK(now - coolerStopped >= SEC(300));
            if (lastActive && lastActive != output) CHECK(now - outputStopped >= SEC(30));
            if (output == CONTROL_OUTPUT_COOLER) CHECK(temperature >= command.targetC + .5f);
            else CHECK(temperature <= command.targetC - .5f);
        }
        previous = output;
    }
    printf("%u control logic checks passed\n", checks);
}
'''

MOCK = r'''
#ifndef CONTROL_MOCK_H
#define CONTROL_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef struct MockQueue *QueueHandle_t;
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NO_MEM 3
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define GPIO_NUM_32 32
#define GPIO_NUM_33 33
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define ESP_LOGI(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) mock_log(tag, __VA_ARGS__)
void mock_log(const char *, const char *, ...);
const char *esp_err_to_name(esp_err_t);
int gpio_set_level(int, int);
int gpio_config(const gpio_config_t *);
int64_t esp_timer_get_time(void);
QueueHandle_t xQueueCreate(unsigned, unsigned);
void vQueueDelete(QueueHandle_t);
int xQueueOverwrite(QueueHandle_t, const void *);
int xQueuePeek(QueueHandle_t, void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *, TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
#endif
'''

ADAPTER_TEST = r'''
#include "TemperatureControl.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC(x) ((int64_t)(x) * 1000000)
#define CHECK(x) do { assert(x); ++checks; } while (0)
struct MockQueue { size_t size; bool full; unsigned char data[128]; };
static struct MockQueue queues[2];
static unsigned queueCount, checks, iteration, writes;
static int heater, cooler;
static int64_t now;
static bool failTask, failWrite, haveSample, failQueue;
static LocalTemperature sample;
static void (*task)(void *);
static jmp_buf finish;
static const char *mode;

void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
int gpio_set_level(int pin, int level) {
    ++writes;
    if (failWrite && pin == RELAY_COOLER && level == ON) return ESP_FAIL;
    if (pin == RELAY_HEATER) heater = level;
    else { CHECK(pin == RELAY_COOLER); cooler = level; }
    CHECK(!(heater == ON && cooler == ON));
    return ESP_OK;
}
int gpio_config(const gpio_config_t *config) {
    CHECK(heater == OFF && cooler == OFF);
    CHECK(config->pin_bit_mask == ((1ULL << 32) | (1ULL << 33)));
    CHECK(config->mode == GPIO_MODE_OUTPUT);
    CHECK(config->pull_up_en == (RELAY_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE));
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return now; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    CHECK(count == 1 && queueCount < 2 && size <= sizeof(queues[0].data));
    if (failQueue && queueCount == 1) return NULL;
    QueueHandle_t q = &queues[queueCount++]; q->size = size; return q;
}
void vQueueDelete(QueueHandle_t q) { q->full = false; }
int xQueueOverwrite(QueueHandle_t q, const void *value) {
    memcpy(q->data, value, q->size); q->full = true; return pdTRUE;
}
int xQueuePeek(QueueHandle_t q, void *value, unsigned wait) {
    CHECK(wait == 0);
    if (!q->full) return 0;
    memcpy(value, q->data, q->size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *value, unsigned wait) {
    int result = xQueuePeek(q, value, wait); q->full = false; return result;
}
TickType_t xTaskGetTickCount(void) { return (TickType_t)(now / 1000); }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    CHECK(!strcmp(name, "temperature_control") && stack >= 3072 && priority == 4);
    CHECK(!arg && !handle);
    if (failTask) return 0;
    task = fn; return pdPASS;
}
bool local_temperature_latest(LocalTemperature *out) {
    if (haveSample) *out = sample;
    return haveSample;
}
static void setSample(float temperature) {
    haveSample = true;
    sample = (LocalTemperature){.temperatureC = temperature, .valid = true,
        .sampledAt = xTaskGetTickCount(), .sampledAtUs = now};
}
static void expect(TemperatureControlState state) {
    TemperatureControlStatus status;
    CHECK(temperature_control_latest(&status));
    CHECK(status.state == state && status.updatedAtUs == now);
    CHECK(status.heaterOn == (heater == ON));
    CHECK(status.coolerOn == (cooler == ON));
    CHECK(!(status.heaterOn && status.coolerOn));
    CHECK(state != CONTROL_SENSOR_FAULT || !status.sensorValid);
}
void vTaskDelayUntil(TickType_t *wake, TickType_t period) {
    CHECK(period == 500 && wake != NULL);
    *wake += period;
    if (!strcmp(mode, "gpio-fault")) {
        switch (iteration++) {
            case 0:
                expect(CONTROL_SENSOR_FAULT); now = SEC(300); setSample(19); failWrite = true; break;
            case 1:
                expect(CONTROL_OUTPUT_FAULT); failWrite = false;
                now += SEC(1000); setSample(10); break;
            case 2:
                expect(CONTROL_OUTPUT_FAULT);
                CHECK(heater == OFF && cooler == OFF);
                CHECK(temperature_control_set_target(18, true));
                now += SEC(1000); setSample(19); break;
            case 3:
                expect(CONTROL_OUTPUT_FAULT); longjmp(finish, 1);
            default: assert(false);
        }
        return;
    }
    switch (iteration++) {
        case 0:
#if CONFIG_BEER_CONTROL_ENABLED
            expect(CONTROL_SENSOR_FAULT);
#else
            expect(CONTROL_DISABLED);
#endif
            CHECK(temperature_control_set_target(18, true));
            setSample(17); break;
        case 1:
            expect(CONTROL_HEATING); now = SEC(1); setSample(18); break;
        case 2:
            expect(CONTROL_IDLE); now = SEC(300); setSample(19); break;
        case 3:
            expect(CONTROL_COOLING); now += SEC(1); sample.valid = false; break;
        case 4:
            expect(CONTROL_SENSOR_FAULT); now += SEC(1); setSample(19); break;
        case 5:
            expect(CONTROL_WAIT_COMPRESSOR);
            CHECK(temperature_control_set_target(19, true));
            CHECK(temperature_control_set_target(20, false)); // last command wins
            now += SEC(1); setSample(19); break;
        case 6: {
            expect(CONTROL_DISABLED);
            TemperatureControlStatus status; CHECK(temperature_control_latest(&status));
            CHECK(status.targetC == 20 && !status.enabled);
            CHECK(!temperature_control_set_target(NAN, true));
            CHECK(!temperature_control_set_target(40, true));
            CHECK(temperature_control_set_target(18, true));
            now = SEC(600); setSample(19); break;
        }
        case 7:
            expect(CONTROL_WAIT_COMPRESSOR); now += SEC(1); setSample(19); break;
        case 8:
            expect(CONTROL_COOLING); now += SEC(5); break; // exactly 5 s is fresh
        case 9:
            expect(CONTROL_COOLING); now += 1; break; // older than 5 s
        case 10:
            expect(CONTROL_SENSOR_FAULT);
            now += SEC(300); setSample(19); sample.sampledAtUs = now + 1; break;
        case 11:
            expect(CONTROL_SENSOR_FAULT);
            // After tick wrap, an ancient sample must stay invalid.
            now += SEC(INT64_C(90) * 86400); sample.sampledAt = xTaskGetTickCount(); break;
        case 12:
            expect(CONTROL_SENSOR_FAULT); setSample(19); break;
        case 13:
            expect(CONTROL_COOLING); sample.temperatureC = NAN; now += SEC(1); break;
        case 14:
            expect(CONTROL_SENSOR_FAULT); setSample(41); break;
        case 15:
            expect(CONTROL_SENSOR_FAULT);
            CHECK(temperature_control_set_target(22, true)); setSample(19); now += SEC(1); break;
        case 16:
            expect(CONTROL_WAIT_CHANGEOVER); now += SEC(29); setSample(19); break;
        case 17:
            expect(CONTROL_HEATING); CHECK(temperature_control_set_target(18, false)); break;
        case 18:
            expect(CONTROL_DISABLED); CHECK(heater == OFF && cooler == OFF);
            longjmp(finish, 1);
        default: assert(false);
    }
}
int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "normal";
    heater = cooler = OFF;
    CHECK(!temperature_control_set_target(18, true));
    CHECK(!temperature_control_latest(NULL));
    CHECK(temperature_control_start() == ESP_ERR_INVALID_STATE);
    failQueue = !strcmp(mode, "queue-fault");
    if (failQueue) {
        CHECK(temperature_control_initialize() == ESP_ERR_NO_MEM);
        CHECK(heater == OFF && cooler == OFF && !commands && !statuses);
        CHECK(temperature_control_start() == ESP_ERR_INVALID_STATE);
    } else {
        CHECK(temperature_control_initialize() == ESP_OK);
        CHECK(heater == OFF && cooler == OFF && writes >= 4);
        CHECK(temperature_control_initialize() == ESP_ERR_INVALID_STATE);
        failTask = !strcmp(mode, "task-fault");
        if (failTask) {
            CHECK(temperature_control_start() == ESP_ERR_NO_MEM);
            CHECK(heater == OFF && cooler == OFF && !started);
        } else {
            CHECK(temperature_control_start() == ESP_OK);
            CHECK(temperature_control_start() == ESP_ERR_INVALID_STATE);
            CHECK(task != NULL);
            if (!setjmp(finish)) task(NULL);
        }
    }
    printf("%u RTOS/GPIO checks passed (active_low=%d, enabled=%d, mode=%s)\n",
           checks, RELAY_ACTIVE_LOW, ENABLED_AT_BOOT, mode);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("Host gcc required; pass its path as an argument.")
    BUILD.mkdir(parents=True, exist_ok=True)
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
              "-I" + str(BUILD), "-I" + str(PROJECT / "main")]
    logic = PROJECT / "main/TemperatureControlLogic.c"
    source = BUILD / "logic_test.c"
    source.write_text(LOGIC_TEST, encoding="utf-8")
    executable = BUILD / "logic_test"
    subprocess.run([*common, str(source), str(logic), "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)

    (BUILD / "control_mock.h").write_text(MOCK, encoding="utf-8")
    for name in ["esp_err.h", "esp_log.h", "esp_timer.h", "sdkconfig.h", "driver/gpio.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "control_mock.h"\n', encoding="utf-8")
    config = "\n".join(
        f"#define CONFIG_BEER_CONTROL_{name} {value}" for name, value in {
            "TARGET_CENTIC": 1800, "MIN_CENTIC": -500, "MAX_CENTIC": 4000,
            "HEATING_DIFF_CENTIC": 50, "COOLING_DIFF_CENTIC": 50,
            "COMPRESSOR_OFF_SECONDS": 300, "CHANGEOVER_SECONDS": 30,
            "SENSOR_MAX_AGE_MS": 5000,
        }.items()
    )
    (BUILD / "sdkconfig.h").write_text(config + "\n", encoding="utf-8")
    source = BUILD / "adapter_test.c"
    source.write_text(ADAPTER_TEST, encoding="utf-8")
    # Adapter includes a copied real source so RelayConfig can test both polarities
    # without changing any firmware file or its existing pin assignments.
    (BUILD / "TemperatureControl.c").write_text(
        (PROJECT / "main/TemperatureControl.c").read_text(encoding="utf-8"), encoding="utf-8")
    for active_low in (1, 0):
        (BUILD / "RelayConfig.h").write_text(
            f'#include "driver/gpio.h"\n#define RELAY_ACTIVE_LOW {active_low}\n'
            '#define RELAY_HEATER GPIO_NUM_32\n#define RELAY_COOLER GPIO_NUM_33\n', encoding="utf-8")
        for enabled in (1, 0):
            executable = BUILD / f"adapter_test_{active_low}_{enabled}"
            definitions = ["-DCONFIG_BEER_CONTROL_ENABLED=1"] if enabled else []
            subprocess.run([*common, *definitions, str(source), str(logic), "-lm", "-o", str(executable)], check=True)
            modes = ("normal", "task-fault", "queue-fault", "gpio-fault") if enabled else ("normal",)
            for mode in modes:
                subprocess.run([str(executable), mode], check=True, timeout=10)


if __name__ == "__main__":
    main()
