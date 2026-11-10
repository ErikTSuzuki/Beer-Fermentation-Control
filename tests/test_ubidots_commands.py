"""Exercise the real Ubidots callback, reassembly and recipe parser, without network.

Run: python tests/test_ubidots_commands.py [path-to-gcc]
Generated files stay in build/ubidots-command-tests. No board is accessed.
"""
from pathlib import Path
import shutil
import subprocess
import sys

from test_control_telemetry import MOCK as MQTT_MOCK, SDK_STUBS
from test_temperature_control import MOCK as CONTROL_MOCK

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/ubidots-command-tests"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

HARNESS = r'''
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

static int64_t wallMs = INT64_C(1800000000000), monotonicUs = INT64_C(10000000);
static int test_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; tv->tv_sec = wallMs / 1000; tv->tv_usec = (wallMs % 1000) * 1000; return 0;
}
static int failMalloc;
static void *command_malloc(size_t size) { return failMalloc ? NULL : malloc(size); }
#define gettimeofday test_gettimeofday
#define malloc command_malloc
#include "UbidotsTelemetry.c"
#undef malloc
#undef gettimeofday

#define CHECK(x) do { assert(x); ++checks; } while (0)
static unsigned checks, subscriptions, starts, cancels, enqueued, refused;
static int bits, subscriptionResult = 1;
static bool running;
static esp_err_t forcedResult;
static FermentationRecipe accepted;
static char lastLog[512];
static const char *recipe =
    "{\"version\":1,\"name\":\"Remota\",\"gravity_unit\":\"sg\","
    "\"steps\":[{\"name\":\"Fermentar\",\"target_c\":20,\"max_days\":5}]}";

void mock_log(const char *tag, const char *format, ...) {
    (void)tag; va_list args; va_start(args, format);
    vsnprintf(lastLog, sizeof(lastLog), format, args); va_end(args);
    if (strstr(lastLog, "enfileirado")) ++enqueued;
    if (strstr(lastLog, "recusado:")) ++refused;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock-error"; }
int64_t esp_timer_get_time(void) { return monotonicUs; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(monotonicUs / 1000); }
bool local_temperature_latest(LocalTemperature *sample) { (void)sample; return false; }
bool ispindel_receiver_latest(IspindelReading *reading) { (void)reading; return false; }
bool temperature_control_latest(TemperatureControlStatus *status) { (void)status; return false; }
bool fermentation_recipe_latest(FermentationRecipeStatus *status) { (void)status; return false; }
const char *temperature_control_state_name(TemperatureControlState state) { (void)state; return "mock"; }
int xEventGroupSetBits(EventGroupHandle_t group, int mask) { (void)group; return bits |= mask; }
int xEventGroupClearBits(EventGroupHandle_t group, int mask) { (void)group; return bits &= ~mask; }
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos) {
    CHECK(client == (void *)1 && !strcmp(topic, COMMAND_TOPIC) && qos == 1);
    ++subscriptions; return subscriptionResult;
}
esp_err_t fermentation_recipe_submit_json(const char *json, size_t length) {
    ++starts;
    if (forcedResult) return forcedResult;
    FermentationRecipe candidate;
    if (!fermentation_recipe_parse(json, length, &candidate)) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < candidate.stepCount; ++i)
        if (candidate.steps[i].targetC < -4.5 || candidate.steps[i].targetC > 39.5)
            return ESP_ERR_INVALID_ARG;
    if (running) return ESP_ERR_INVALID_STATE;
    accepted = candidate; running = true; return ESP_OK;
}
esp_err_t fermentation_recipe_cancel(void) {
    ++cancels;
    if (forcedResult) return forcedResult;
    running = false; return ESP_OK;
}
static void advance(int ms) { wallMs += ms; monotonicUs += (int64_t)ms * 1000; }
static void event(int type) {
    __typeof__(*(esp_mqtt_event_handle_t)0) data = {.client = (void *)1};
    onMqttEvent(NULL, NULL, type, &data);
}
static void fragment(const char *topic, const char *body, int length, int offset, int total, bool retained) {
    __typeof__(*(esp_mqtt_event_handle_t)0) data = {
        .topic = (char *)topic, .topic_len = topic ? (int)strlen(topic) : 0,
        .data = (char *)body, .data_len = length, .total_data_len = total,
        .current_data_offset = offset, .retain = retained};
    onMqttEvent(NULL, NULL, MQTT_EVENT_DATA, &data);
}
static void send(const char *body) {
    fragment(COMMAND_TOPIC, body, (int)strlen(body), 0, (int)strlen(body), false);
    CHECK(!commandBody);
}
static char *message(double action, const char *profile, double stamp) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "value", action);
    cJSON_AddNumberToObject(root, "timestamp", stamp);
    if (profile) {
        cJSON *context = cJSON_AddObjectToObject(root, "context");
        cJSON_AddStringToObject(context, "recipe", profile);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); CHECK(json); return json;
}
static void command(double action, const char *profile) {
    advance(1000); char *json = message(action, profile, (double)wallMs);
    send(json); free(json);
}
static void noCall(const char *json) {
    unsigned before = starts + cancels; send(json); CHECK(starts + cancels == before);
}
int main(void) {
    event(MQTT_EVENT_CONNECTED); CHECK(subscriptions == 1 && bits == MQTT_CONNECTED_BIT);
    event(MQTT_EVENT_SUBSCRIBED); CHECK(strstr(lastLog, "confirmada"));
    // Latest stored value, including one from earlier in this connection's millisecond.
    char *json = message(1, recipe, (double)wallMs); noCall(json); free(json);
    command(1, recipe); CHECK(starts == 1 && running && enqueued == 1);
    CHECK(!strcmp(accepted.name, "Remota") && accepted.steps[0].targetC == 20);
    // The same timestamp is consumed once, even if broker re-delivers it.
    json = message(1, recipe, (double)wallMs); noCall(json); free(json);
    command(1, recipe); CHECK(starts == 2 && running && refused == 1);
    command(2, NULL); CHECK(cancels == 1 && !running && enqueued == 2);

    // Fragmented message larger than the client's 2048-byte receive buffer.
    char largeRecipe[RECIPE_JSON_MAX + 1];
    memset(largeRecipe, ' ', sizeof(largeRecipe));
    memcpy(largeRecipe, recipe, strlen(recipe)); largeRecipe[RECIPE_JSON_MAX] = 0;
    advance(1000); json = message(1, largeRecipe, (double)wallMs);
    const int length = (int)strlen(json); CHECK(length > 2048);
    unsigned before = starts;
    for (int at = 0; at < length;) {
        const int chunk = length - at < 197 ? length - at : 197;
        fragment(at ? NULL : COMMAND_TOPIC, json + at, chunk, at, length, false);
        at += chunk;
        CHECK(starts == before + (at == length ? 1 : 0));
    }
    CHECK(!commandBody && running); free(json); command(2, NULL);

    // Invalid JSON, shape, action, embedded NUL, duplicated fields and recipe types.
    noCall("{}"); noCall("{"); noCall("[]"); noCall("1"); noCall("{} trailing");
    noCall("{\"value\":1,\"value\":2,\"timestamp\":1800000010000}");
    noCall("{\"value\":2,\"timestamp\":1800000010000,\"timestamp\":1800000010001}");
    noCall("{\"value\":2,\"timestamp\":1800000010000,\"context\":{\"a\":1,\"a\":2}}");
    noCall("{\"value\":2,\"timestamp\":1800000010000,\"context\":{\"a\":{\"b\":{}}}}");
    noCall("{\"value\":2,\"timestamp\":1800000010000,\"context\":{\"a\":\"\\u0000\"}}");
    noCall("{\"value\":1,\"timestamp\":1800000010000,\"context\":{\"recipe\":{}}}");
    advance(1000);
    const double invalidActions[] = {0, 3, -1, 1.5};
    for (unsigned i = 0; i < sizeof(invalidActions)/sizeof(invalidActions[0]); ++i) {
        json = message(invalidActions[i], recipe, (double)wallMs); noCall(json); free(json);
    }
    json = message(1, NULL, (double)wallMs); noCall(json); free(json);
    json = message(1, "", (double)wallMs); noCall(json); free(json);
    char oversized[RECIPE_JSON_MAX + 2]; memset(oversized, ' ', sizeof(oversized));
    oversized[sizeof(oversized)-1] = 0;
    json = message(1, oversized, (double)wallMs); noCall(json); free(json);

    // Invalid recipe reaches the existing validator and never changes the run.
    before = refused; command(1, "{}"); CHECK(!running && refused == before + 1);
    command(1, "{\"version\":1,\"name\":\"bad\",\"gravity_unit\":\"sg\",\"steps\":[{\"name\":\"bad\",\"target_c\":50,\"max_days\":1}]}");
    CHECK(!running && refused == before + 2);
    forcedResult = ESP_ERR_NO_MEM; command(2, NULL); CHECK(refused == before + 3);
    forcedResult = ESP_OK;
    json = message(2, NULL, (double)wallMs); noCall(json); free(json);

    // Freshness: same timestamp, future, fractional, negative, huge, expired, no NTP.
    advance(1000);
    double stamps[] = {(double)wallMs+1, (double)wallMs-0.5, -1, 1e30, (double)wallMs-120001};
    for (unsigned i = 0; i < sizeof(stamps)/sizeof(stamps[0]); ++i) {
        json = message(2, NULL, stamps[i]); noCall(json); free(json);
    }
    int64_t savedWall = wallMs; wallMs = 1000;
    json = message(2, NULL, (double)savedWall); noCall(json); free(json); wallMs = savedWall;

    // Exercise the TTL independently of the connection and duplicate cutoffs.
    advance(200000);
    json = message(2, NULL, (double)(wallMs-120001)); noCall(json); free(json);
    before = cancels;
    json = message(2, NULL, (double)(wallMs-120000)); send(json); free(json);
    CHECK(cancels == before + 1);

    // Parser allocation failures must leave the active run untouched.
    cJSON_Hooks hooks = {.malloc_fn = command_malloc, .free_fn = free};
    advance(1000); json = message(1, recipe, (double)wallMs);
    before = starts; failMalloc = 1; cJSON_InitHooks(&hooks);
    dispatchCommand(json, strlen(json));
    cJSON_InitHooks(NULL); failMalloc = 0; free(json);
    CHECK(starts == before && !running);

    // Wrong topic, retained commands, oversized/inconsistent bodies, missing chunks.
    advance(1000); json = message(2, NULL, (double)wallMs); before = cancels;
    fragment("/v1.6/devices/other/comando-receita", json, strlen(json), 0, strlen(json), false);
    fragment(COMMAND_TOPIC, json, strlen(json), 0, strlen(json), true);
    fragment(COMMAND_TOPIC, "x", 1, 0, COMMAND_BODY_MAX+1, false);
    fragment(COMMAND_TOPIC, "", 0, 0, 0, false);
    fragment(COMMAND_TOPIC, json, 10, 0, strlen(json), false); CHECK(commandBody);
    fragment(NULL, json+11, strlen(json)-11, 11, strlen(json), false); CHECK(!commandBody);
    fragment(COMMAND_TOPIC, json, 10, 0, strlen(json), false);
    fragment(NULL, json+10, strlen(json)-10, 10, strlen(json)+1, false); CHECK(!commandBody);
    fragment(COMMAND_TOPIC, json, strlen(json), 0, 2, false); CHECK(!commandBody);
    fragment(COMMAND_TOPIC, NULL, 1, 0, 1, false); CHECK(!commandBody);
    fragment(NULL, json, strlen(json), -1, strlen(json), false);
    const char nulBody[] = "{\"value\":2}\0{}";
    fragment(COMMAND_TOPIC, nulBody, sizeof(nulBody)-1, 0, sizeof(nulBody)-1, false);
    failMalloc = 1; send(json); failMalloc = 0;
    CHECK(!commandBody && cancels == before);

    // Disconnect discards an unfinished body; reconnect subscribes again and
    // refuses values stored while offline, even if not marked retain.
    fragment(COMMAND_TOPIC, json, 10, 0, strlen(json), false); CHECK(commandBody);
    event(MQTT_EVENT_DISCONNECTED); CHECK(!commandBody && !bits);
    noCall(json); advance(1000); event(MQTT_EVENT_CONNECTED);
    CHECK(subscriptions == 2); noCall(json); free(json);
    command(2, NULL); CHECK(cancels == before + 1);
    event(MQTT_EVENT_ERROR); CHECK(!commandConnected && !bits);
    subscriptionResult = -1; event(MQTT_EVENT_CONNECTED);
    CHECK(strstr(lastLog, "Falha ao assinar"));

    // A connection opened before NTP still rejects its stored value after sync.
    wallMs = 1000; event(MQTT_EVENT_CONNECTED); advance(1000);
    wallMs = savedWall + 100000;
    json = message(2, NULL, (double)(wallMs-2000)); noCall(json); free(json);
    command(2, NULL); CHECK(!commandBody);
    printf("%u Ubidots command checks passed (callback, fragments, freshness, duplicates, recipes, reconnect)\n", checks);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("Host gcc required; pass its path as an argument.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "control_mock.h").write_text(CONTROL_MOCK, encoding="utf-8")
    (BUILD / "telemetry_mock.h").write_text(
        "#ifndef TELEMETRY_MOCK_H\n#define TELEMETRY_MOCK_H\n" + MQTT_MOCK + "\n#endif\n", encoding="utf-8")
    for name in ["sdkconfig.h", "esp_wifi.h", "mqtt_client.h", "esp_err.h", "esp_log.h",
                 "esp_timer.h", "freertos/event_groups.h", "freertos/FreeRTOS.h", "freertos/task.h"]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "telemetry_mock.h"\n', encoding="utf-8")
    source = BUILD / "commands_test.c"
    source.write_text(HARNESS + SDK_STUBS, encoding="utf-8")
    executable = BUILD / "commands_test"
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-I" + str(BUILD), "-I" + str(PROJECT / "main"), "-I" + str(CJSON),
        str(source), str(PROJECT / "main/FermentationRecipeJson.c"),
        str(PROJECT / "main/FermentationRecipeLogic.c"), str(CJSON / "cJSON.c"),
        "-Wl,--gc-sections", "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
