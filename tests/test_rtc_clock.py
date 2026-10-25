"""Exercise RTC writes, Brasilia conversion and offline/online task behavior.

Run: python tests/test_rtc_clock.py [path-to-gcc]
Generated files stay in build/rtc-tests.
"""
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/rtc-tests"

MOCK = r'''
#ifndef RTC_MOCK_H
#define RTC_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef struct MockQueue *QueueHandle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct {
    bool wait_for_sync;
    void (*sync_cb)(struct timeval *);
    const char *server;
} esp_sntp_config_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_NO_MEM 4
#define pdTRUE 1
#define pdPASS 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define GPIO_NUM_5 5
#define GPIO_NUM_15 15
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) {true, NULL, s}
#define ESP_LOGI(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) mock_log(tag, __VA_ARGS__)
void mock_log(const char *, const char *, ...);
const char *esp_err_to_name(esp_err_t);
QueueHandle_t xQueueCreate(unsigned, unsigned);
int xQueueOverwrite(QueueHandle_t, const void *);
int xQueuePeek(QueueHandle_t, void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
void vQueueDelete(QueueHandle_t);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
int i2c_master_transmit_receive(void *, const uint8_t *, size_t, uint8_t *, size_t, int);
int i2c_master_transmit(void *, const uint8_t *, size_t, int);
int i2c_new_master_bus(const i2c_master_bus_config_t *, void **);
int i2c_master_bus_add_device(void *, const i2c_device_config_t *, void **);
int i2c_master_bus_rm_device(void *);
int i2c_del_master_bus(void *);
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_netif_sntp_start(void);
#endif
'''

HARNESS = r'''
#include "RtcClock.c"
#include <setjmp.h>
#include <stdarg.h>

struct MockQueue { size_t size; bool full; uint8_t value[128]; };
static struct MockQueue queues[2];
static unsigned queueCount;
static uint8_t registers[256];
static bool connected, writeFail, failInit, failQueue;
static unsigned timeWrites, initCalls, restartCalls, stage, checks;
static TickType_t ticks;
static time_t fakeTime = TEST_UTC;
static void (*task)(void *);
static jmp_buf finish;
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
static void (*notify)(struct timeval *);
#endif
#define CHECK(expr) do { assert(expr); ++checks; } while (0)

void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t value) { (void)value; return "mock-error"; }
time_t time(time_t *out) { if (out) *out = fakeTime; return fakeTime; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    assert(count == 1 && queueCount < 2 && size <= sizeof(queues[0].value));
    if (queueCount == 1 && failQueue) return NULL;
    QueueHandle_t q = &queues[queueCount++]; q->size = size; return q;
}
int xQueueOverwrite(QueueHandle_t q, const void *value) {
    memcpy(q->value, value, q->size); q->full = true; return pdTRUE;
}
int xQueuePeek(QueueHandle_t q, void *out, unsigned wait) {
    (void)wait; if (!q->full) return 0;
    memcpy(out, q->value, q->size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *out, unsigned wait) {
    const int result = xQueuePeek(q, out, wait); q->full = false; return result;
}
void vQueueDelete(QueueHandle_t q) { q->full = false; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    task = fn; return pdPASS;
}
int i2c_new_master_bus(const i2c_master_bus_config_t *config, void **bus) {
    assert(config->sda_io_num == 5 && config->scl_io_num == 15);
    *bus = registers; return ESP_OK;
}
int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *config, void **out) {
    assert(bus == registers && config->device_address == 0x68 && config->scl_speed_hz == 100000);
    *out = registers; return ESP_OK;
}
int i2c_master_bus_rm_device(void *dev) { (void)dev; return ESP_OK; }
int i2c_del_master_bus(void *bus) { (void)bus; return ESP_OK; }
int i2c_master_transmit_receive(void *dev, const uint8_t *address, size_t addressSize, uint8_t *out, size_t size, int timeout) {
    assert(dev == registers && addressSize == 1 && timeout == 100);
    assert((size_t)*address + size <= sizeof(registers));
    memcpy(out, registers + *address, size); return ESP_OK;
}
int i2c_master_transmit(void *dev, const uint8_t *data, size_t size, int timeout) {
    assert(dev == registers && timeout == 100 && size >= 2);
    if (writeFail) return ESP_FAIL;
    if (data[0] == 0 && size == 8) ++timeWrites;
    memcpy(registers + data[0], data + 1, size - 1); return ESP_OK;
}
bool wifi_station_is_connected(void) { return connected; }
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
static void notification(void) {
    const unsigned before = timeWrites;
    struct timeval tv = {.tv_sec = fakeTime}; notify(&tv);
    CHECK(timeWrites == before); // Callback does not touch I2C.
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    CHECK(connected && !config->wait_for_sync);
    CHECK(!strcmp(config->server, "pool.ntp.org"));
    ++initCalls;
    if (failInit) return ESP_FAIL;
    notify = config->sync_cb; notification(); return ESP_OK;
}
esp_err_t esp_netif_sntp_start(void) {
    CHECK(connected); ++restartCalls; notification(); return ESP_OK;
}
#endif
static void expectTimestamp(const char *expected) {
    char text[40]; CHECK(rtc_clock_timestamp(text, sizeof(text)));
    CHECK(!strcmp(text, expected));
}
void vTaskDelay(TickType_t delay) {
    CHECK(delay == 1000); ticks += delay; ++stage;
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
    if (failInit || failQueue) {
        expectTimestamp("2020-01-02 03:04:05");
        CHECK(timeWrites == 0);
        if (failQueue) {
            CHECK(syncRequests == NULL && initCalls == 0);
            longjmp(finish, 1);
        }
        if (stage == 1) connected = true;
        else if (stage == 2) {
            CHECK(initCalls == 1);
        } else if (stage == 3) {
            CHECK(initCalls == 1); // No busy loop while NTP init keeps failing.
            ticks += 60000;
        } else if (stage == 4) {
            CHECK(initCalls == 2); longjmp(finish, 1);
        } else assert(false);
        return;
    }
#endif
    switch (stage) {
        case 1:
            expectTimestamp("2020-01-02 03:04:05");
            CHECK(initCalls == 0 && restartCalls == 0 && timeWrites == 0);
#if CONFIG_BEER_WIFI_UPLINK_ENABLED
            connected = true;
            break;
        case 2:
            expectTimestamp("2026-10-24 22:02:03");
            CHECK(initCalls == 1 && timeWrites == 1);
            connected = false; registers[0] = toBcd(4);
            break;
        case 3:
            expectTimestamp("2026-10-24 22:02:04");
            CHECK(timeWrites == 1);
            connected = true; fakeTime += 61;
            break;
        case 4:
            expectTimestamp("2026-10-24 22:03:04");
            CHECK(restartCalls == 1 && timeWrites == 2);
            fakeTime += 60; writeFail = true; notification();
            break;
        case 5:
            expectTimestamp("2026-10-24 22:03:04");
            CHECK(timeWrites == 2); writeFail = false;
            break;
        case 6:
            expectTimestamp("2026-10-24 22:04:04");
            CHECK(timeWrites == 3);
            fakeTime = TEST_NEW_YEAR; notification();
            break;
        case 7:
            expectTimestamp("2026-12-31 22:02:03");
            CHECK(timeWrites == 4);
            fakeTime = 0; notification();
            break;
        case 8:
            expectTimestamp("2026-12-31 22:02:03");
            CHECK(timeWrites == 4); fakeTime = TEST_NEW_YEAR + 60;
            break;
        case 9:
            expectTimestamp("2026-12-31 22:03:03");
            CHECK(timeWrites == 5); longjmp(finish, 1);
#else
            longjmp(finish, 1);
#endif
        default: assert(false);
    }
}
int main(int argc, char **argv) {
    failInit = argc > 1 && !strcmp(argv[1], "fail-ntp");
    failQueue = argc > 1 && !strcmp(argv[1], "fail-queue");
    device = registers;
    DateTime leap = {2024, 2, 29, 12, 34, 56};
    registers[0x0e] = 0x9c; registers[0x0f] = 0x8b;
    CHECK(setTime(&leap) == ESP_OK);
    CHECK(registers[0x0e] == 0x1c && registers[0x0f] == 0x0b);
    CHECK(registers[0] == 0x56 && registers[1] == 0x34 && registers[2] == 0x12);
    CHECK(registers[3] == 5); // Thursday, Sunday = 1.
    DateTime read; bool invalid;
    CHECK(readTime(&read, &invalid) == ESP_OK && !invalid);
    CHECK(read.year == 2024 && read.month == 2 && read.day == 29);
    leap.year = 2025; CHECK(setTime(&leap) == ESP_ERR_INVALID_ARG);
    leap = (DateTime){2100, 1, 1, 0, 0, 0};
    CHECK(setTime(&leap) == ESP_ERR_INVALID_ARG);
    const DateTime original = {2020, 1, 2, 3, 4, 5};
    CHECK(setTime(&original) == ESP_OK); timeWrites = 0;
    rtc_clock_start(); CHECK(task != NULL);
    if (!setjmp(finish)) task(NULL);
    char text[40]; ticks += 3000;
    CHECK(!rtc_clock_timestamp(text, sizeof(text)));
    CHECK(!strcmp(text, "RTC INDISPONIVEL"));
    printf("%u RTC checks passed (network=%d, mode=%s)\n", checks,
           CONFIG_BEER_WIFI_UPLINK_ENABLED, argc > 1 ? argv[1] : "normal");
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("Host gcc required; pass its path as an argument.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "rtc_mock.h").write_text(MOCK, encoding="utf-8")
    for name in [
        "driver/gpio.h", "driver/i2c_master.h", "esp_log.h", "sdkconfig.h",
        "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h", "esp_netif_sntp.h",
    ]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "rtc_mock.h"\n', encoding="utf-8")
    source = BUILD / "rtc_test.c"
    source.write_text(HARNESS, encoding="utf-8")
    utc = int(datetime(2026, 10, 25, 1, 2, 3, tzinfo=timezone.utc).timestamp())
    new_year = int(datetime(2027, 1, 1, 1, 2, 3, tzinfo=timezone.utc).timestamp())
    for network in (1, 0):
        executable = BUILD / f"rtc_test_network_{network}"
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
            f"-DCONFIG_BEER_WIFI_UPLINK_ENABLED={network}", f"-DTEST_UTC={utc}", f"-DTEST_NEW_YEAR={new_year}",
            "-I" + str(BUILD), "-I" + str(PROJECT / "main"), str(source), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
        if network:
            for mode in ("fail-ntp", "fail-queue"):
                subprocess.run([str(executable), mode], check=True, timeout=10)


if __name__ == "__main__":
    main()
