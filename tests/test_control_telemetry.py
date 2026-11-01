"""Check real MQTT payload serialization, including controller-only fault reports.

Run after test_temperature_control.py. Uses its host SDK stubs, no network.
"""
from pathlib import Path
import json
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/control-tests/telemetry"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

MOCK = r'''
#include "control_mock.h"
typedef void *EventGroupHandle_t;
typedef void *esp_mqtt_client_handle_t;
typedef const char *esp_event_base_t;
typedef struct {
    struct { struct { const char *uri; } address; } broker;
    struct { const char *client_id, *username; struct { const char *password; } authentication; } credentials;
    struct { int protocol_ver, keepalive; } session;
    struct { int reconnect_timeout_ms, timeout_ms; } network;
    struct { int size; } buffer;
    struct { int priority, stack_size; } task;
} esp_mqtt_client_config_t;
typedef struct { int error_type, connect_return_code; } mock_mqtt_error;
typedef struct { mock_mqtt_error *error_handle; } *esp_mqtt_event_handle_t;
#define BIT0 1
#define WIFI_IF_STA 0
#define configTICK_RATE_HZ 1000
#define CONFIG_BEER_ISPINDEL_STALE_SECONDS 1800
#define CONFIG_BEER_UBIDOTS_ENABLED 1
#define CONFIG_BEER_UBIDOTS_DEVICE_LABEL "test"
#define CONFIG_BEER_UBIDOTS_TOKEN "test"
#define CONFIG_BEER_WIFI_SSID "test"
#define CONFIG_BEER_UBIDOTS_INTERVAL_SECONDS 300
#define ESP_LOGW(tag, ...) mock_log(tag, __VA_ARGS__)
#define MQTT_EVENT_CONNECTED 1
#define MQTT_EVENT_DISCONNECTED 2
#define MQTT_EVENT_ERROR 3
#define MQTT_ERROR_TYPE_CONNECTION_REFUSED 1
#define MQTT_PROTOCOL_V_3_1_1 4
#define ESP_EVENT_ANY_ID -1
int xEventGroupSetBits(EventGroupHandle_t, int);
int xEventGroupClearBits(EventGroupHandle_t, int);
int xEventGroupGetBits(EventGroupHandle_t);
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t);
esp_err_t esp_wifi_get_mac(int, uint8_t *);
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int,
    void (*)(void *, esp_event_base_t, int32_t, void *), void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, int);
void vTaskDelete(void *);
void vTaskDelay(TickType_t);
'''

HARNESS = r'''
#include "UbidotsTelemetry.c"
#include <assert.h>
#include <stdlib.h>

static bool localAvailable, remoteAvailable, controlAvailable;
static const int64_t now = INT64_C(10000000);
static int scenario;
void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
int64_t esp_timer_get_time(void) { return now; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(now / 1000); }
bool local_temperature_latest(LocalTemperature *sample) {
    *sample = (LocalTemperature){.valid = true, .temperatureC = 19.123f,
        .sampledAt = xTaskGetTickCount(), .sampledAtUs = now};
    return localAvailable;
}
bool ispindel_receiver_latest(IspindelReading *reading) {
    *reading = (IspindelReading){.packet = {
        .header = {.gravityUnit = BREW_UNIT_SG, .sensorId = UINT32_MAX},
        .temperatureC = 19.5f, .tiltDegrees = 45, .gravity = 1.020f, .batteryV = 4.1f},
        .receivedAt = xTaskGetTickCount()};
    return remoteAvailable;
}
bool temperature_control_latest(TemperatureControlStatus *status) {
    *status = (TemperatureControlStatus){.state = CONTROL_SENSOR_FAULT, .targetC = 18,
        .enabled = true, .sensorValid = false, .waitSeconds = 299, .updatedAtUs = now};
    if (scenario == 4) status->updatedAtUs -= 2000001;
    if (scenario == 5) status->updatedAtUs += 1;
    if (scenario == 6) status->updatedAtUs -= 2000000;
    return controlAvailable;
}
bool fermentation_recipe_latest(FermentationRecipeStatus *status) {
    *status = (FermentationRecipeStatus){.state=RECIPE_RUNNING, .stepIndex=1, .stepCount=3,
        .elapsedDays=1.5, .maximumDays=5, .gravityAvailable=false, .storageOk=true,
        .hasGravityLimit=true, .gravityLimit=1.01f, .reason=RECIPE_REASON_TIME, .updatedAtUs=now};
    strcpy(status->recipeName, "Receita \"especial\"\\\n");
    strcpy(status->stepName, "Segunda etapa");
    if (scenario==9) status->updatedAtUs=now+1;
    if (scenario==10) status->updatedAtUs=now-2500001;
    if (scenario==11) status->updatedAtUs=now-2500000;
    return scenario>=8;
}
const char *temperature_control_state_name(TemperatureControlState state) {
    assert(state == CONTROL_SENSOR_FAULT); return "FALHA SENSOR";
}
int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]);
    localAvailable = scenario == 0 || scenario == 2 || scenario == 3 || scenario == 4 || scenario==12;
    remoteAvailable = scenario == 0 || scenario == 2 || scenario==12;
    controlAvailable = (scenario != 2 && scenario<8) || scenario==12;
    if (scenario == 7) controlAvailable = false;
    char payload[2048];
    const int dots = buildPayload(payload, sizeof(payload));
    if (scenario == 7 || scenario == 5 || scenario==9 || scenario==10) { assert(dots == 0); puts("null"); return 0; }
    assert(dots == (localAvailable ? 1 : 0) + (remoteAvailable ? 4 : 0) +
        (controlAvailable && scenario != 4 ? 2 : 0) + (scenario>=8 ? 3 : 0));
    // Exact capacity must fit; shorter capacities must return a failure.
    const size_t needed = strlen(payload) + 1;
    for (size_t size = 0; size < needed; ++size) {
        char small[2048]; assert(buildPayload(small, size) == -1);
    }
    char exact[2048]; assert(buildPayload(exact, needed) == dots);
    assert(!strcmp(payload, exact));
    puts(payload);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    mock_path = BUILD.parent / "control_mock.h"
    if not compiler or not mock_path.exists():
        raise SystemExit("gcc required; run test_temperature_control.py first.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "telemetry_mock.h").write_text('#ifndef TELEMETRY_MOCK_H\n#define TELEMETRY_MOCK_H\n' + MOCK + '\n#endif\n', encoding="utf-8")
    for name in ["sdkconfig.h", "esp_wifi.h", "mqtt_client.h", "freertos/event_groups.h"]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "telemetry_mock.h"\n', encoding="utf-8")
    source = BUILD / "telemetry_test.c"
    source.write_text(HARNESS, encoding="utf-8")
    executable = BUILD / "telemetry_test"
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-I" + str(BUILD), "-I" + str(BUILD.parent), "-I" + str(PROJECT / "main"),
        "-I" + str(CJSON), str(source), str(CJSON / "cJSON.c"), "-Wl,--gc-sections", "-lm", "-o", str(executable),
    ], check=True)
    for scenario in range(13):
        payload = json.loads(subprocess.run([str(executable), str(scenario)], check=True,
            capture_output=True, text=True, timeout=5).stdout)
        if scenario in (5, 7, 9, 10):
            assert payload is None
            continue
        if scenario in (2, 4, 8, 11):
            assert "estado-controle" not in payload and "temperatura-alvo" not in payload
        else:
            assert payload["temperatura-alvo"] == 18
            state = payload["estado-controle"]
            assert state["value"] == 6
            assert state["context"] == {
                "estado": "FALHA SENSOR", "habilitado": True, "sensor_valido": False,
                "aquecedor": False, "refrigerador": False, "espera_s": 299,
            }
        if scenario == 0:
            assert len(payload) == 7 and payload["densidade"]["context"]["unit"] == "sg"
        if scenario in (1, 6):
            assert len(payload) == 2
        if scenario in (8,11,12):
            assert payload['etapa-fermentacao']['value']==2
            context=payload['etapa-fermentacao']['context']
            assert context['receita']=='Receita "especial"\\\n'
            assert context['estado']==1 and context['etapa']=='Segunda etapa'
            assert payload['dias-etapa']==1.5 and payload['dias-max-etapa']==5
            assert len(payload)==(10 if scenario==12 else 3)
    print("13 control/recipe telemetry scenarios passed (JSON, escaping, faults, freshness and buffer bounds)")


if __name__ == "__main__":
    main()
