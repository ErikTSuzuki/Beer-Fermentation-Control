"""Exercise the broker callback and recipe decoder with a simulated ESP SDK."""
from pathlib import Path
import shutil
import subprocess
import sys

from test_control_telemetry import MOCK, SDK_STUBS
from test_temperature_control import MOCK as CONTROL_MOCK

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/mqtt-command-tests"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

HARNESS = r'''
#include "BrokerMqtt.c"
#include <assert.h>

static int64_t nowUs = 20000000;
static unsigned subscriptions, submissions, results;
static bool queued;
static IncomingCommand incoming;
static char lastResult[192];
static const char session[] = "0123456789abcdef0123456789abcdef";
static const char profile[] =
    "{\"version\":1,\"name\":\"Remota\",\"gravity_unit\":\"sg\","
    "\"steps\":[{\"name\":\"Fermentar\",\"target_c\":20,\"max_days\":5}]}";

void mock_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return nowUs; }
int xQueueSend(QueueHandle_t queue, const void *value, unsigned ticks) {
    (void)queue; (void)ticks;
    assert(!queued);
    incoming = *(const IncomingCommand *)value;
    queued = true;
    return pdTRUE;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos) {
    assert(client && !strcmp(topic, COMMAND_TOPIC) && qos == 1);
    return ++subscriptions;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic,
                            const char *body, int length, int qos, int retain, bool store) {
    (void)store; (void)length;
    assert(client && !strcmp(topic, STATE_TOPIC) && qos == 1 && !retain);
    ++results;
    strcpy(lastResult, body);
    return 1;
}
esp_err_t fermentation_recipe_submit_remote(const char *json, size_t length,
    uint32_t revision, uint32_t ticket, int64_t expiresAtUs) {
    assert(ticket && revision == 7 && expiresAtUs == 33000000);
    ++submissions;
    if (json) {
        FermentationRecipe candidate;
        if (!fermentation_recipe_parse(json, length, &candidate)) return ESP_ERR_INVALID_ARG;
        assert(!strcmp(candidate.name, "Remota") && candidate.steps[0].targetC == 20);
    }
    return ESP_OK;
}

static void event(int type) {
    __typeof__(*(esp_mqtt_event_handle_t)0) data = {.client = (void *)1, .msg_id = (int)subscriptions};
    onMqttEvent(NULL, NULL, type, &data);
}
static void fragment(const char *topic, const char *body, int length, int offset, int total, bool retained) {
    __typeof__(*(esp_mqtt_event_handle_t)0) data = {
        .topic = (char *)topic, .topic_len = topic ? (int)strlen(topic) : 0,
        .data = (char *)body, .data_len = length, .total_data_len = total,
        .current_data_offset = offset, .retain = retained};
    onMqttEvent(NULL, NULL, MQTT_EVENT_DATA, &data);
}
static void dispatch(PendingCommand *pending) {
    assert(queued && !commandBody);
    dispatchCommand((void *)1, &incoming, session, pending);
    free(incoming.body);
    queued = false;
}
static void message(char *body, unsigned id, const char *token, unsigned lease,
                    const char *action, const char *recipe) {
    const int length = snprintf(body, COMMAND_BODY_MAX,
        "{\"id\":\"00000000-0000-0000-0000-%012x\",\"session\":\"%s\","
        "\"revision\":7,\"lease\":%u,\"action\":\"%s\"%s%s}",
        id, token, lease, action, recipe ? ",\"recipe\":" : "", recipe ? recipe : "");
    assert(length > 0 && length < COMMAND_BODY_MAX);
}
static void send(const char *body) {
    const int length = strlen(body);
    fragment(COMMAND_TOPIC, body, length, 0, length, false);
}

int main(void) {
    PendingCommand pending = {0};
    char body[COMMAND_BODY_MAX];
    message(body, 1, session, 20, "start", profile);
    event(MQTT_EVENT_CONNECTED);
    send(body);
    assert(!queued && subscriptions == 1);
    event(MQTT_EVENT_SUBSCRIBED);
    const int length = strlen(body);
    fragment(COMMAND_TOPIC, body, length, 0, length, true);
    fragment("another/device", body, length, 0, length, false);
    fragment(COMMAND_TOPIC, body, 1, 0, COMMAND_BODY_MAX + 1, false);
    assert(!queued);
    fragment(COMMAND_TOPIC, body, 100, 0, length, false);
    assert(commandBody && !queued);
    fragment(NULL, body + 100, length - 100, 100, length, false);
    dispatch(&pending);
    assert(submissions == 1 && pending.ticket && !pending.cancel && results == 0);

    send(body);
    dispatch(&pending);
    assert(submissions == 1 && results == 0);
    message(body, 2, session, 20, "cancel", NULL);
    send(body);
    dispatch(&pending);
    assert(submissions == 1 && results == 1 && strstr(lastResult, "busy"));
    pending.ticket = 0;
    send(body);
    dispatch(&pending);
    assert(submissions == 1 && results == 1);

    message(body, 3, session, 7, "cancel", NULL);
    send(body);
    dispatch(&pending);
    message(body, 4, "session-before-reconnect", 20, "cancel", NULL);
    send(body);
    dispatch(&pending);
    assert(submissions == 1 && results == 1);
    message(body, 5, session, 20, "start", "{}");
    send(body);
    dispatch(&pending);
    assert(submissions == 2 && results == 2 && strstr(lastResult, "invalid_recipe"));

    message(body, 6, session, 20, "cancel", NULL);
    fragment(COMMAND_TOPIC, body, 30, 0, strlen(body), false);
    fragment(NULL, body + 31, strlen(body) - 31, 31, strlen(body), false);
    assert(!commandBody && !queued);
    fragment(COMMAND_TOPIC, body, 30, 0, strlen(body), false);
    event(MQTT_EVENT_DISCONNECTED);
    assert(!commandBody && !(atomic_load(&connection) & 1));
    send(body);
    assert(!queued);
    event(MQTT_EVENT_CONNECTED);
    event(MQTT_EVENT_SUBSCRIBED);
    assert(subscriptions == 2);
    send(body);
    dispatch(&pending);
    assert(submissions == 3 && pending.cancel);
    publishResult((void *)1, pending.id, ESP_OK, true);
    assert(strstr(lastResult, pending.id) && strstr(lastResult, "\"storage_ok\":true"));
    puts("MQTT commands passed (subscription, fragments, expiry, duplicates, invalid recipe, result and reconnect)");
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("Host gcc required.")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "control_mock.h").write_text(CONTROL_MOCK, encoding="utf-8")
    (BUILD / "telemetry_mock.h").write_text(
        "#ifndef TELEMETRY_MOCK_H\n#define TELEMETRY_MOCK_H\n" + MOCK + "\n#endif\n", encoding="utf-8")
    for name in ("sdkconfig.h", "esp_wifi.h", "mqtt_client.h", "esp_err.h", "esp_log.h",
                 "esp_timer.h", "esp_random.h", "freertos/queue.h", "freertos/FreeRTOS.h", "freertos/task.h"):
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "telemetry_mock.h"\n', encoding="utf-8")
    source = BUILD / "commands_test.c"
    source.write_text(HARNESS + SDK_STUBS, encoding="utf-8")
    executable = BUILD / "commands_test"
    subprocess.run([
        compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-I" + str(BUILD), "-I" + str(PROJECT / "main"),
        "-I" + str(CJSON), str(source), str(PROJECT / "main/FermentationRecipeJson.c"),
        str(PROJECT / "main/FermentationRecipeLogic.c"), str(CJSON / "cJSON.c"),
        "-Wl,--gc-sections", "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
