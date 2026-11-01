"""Exercise real recipe parser, timing logic and RTOS/NVS adapter on the host.

Run: python3 tests/test_fermentation_recipe.py [path-to-gcc]
Build artifacts stay in build/recipe-tests. Does not access a board or network.
"""
from copy import deepcopy
from pathlib import Path
import json
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/recipe-tests"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

LOGIC = r'''
#include "FermentationRecipeLogic.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { assert(x); ++checks; } while (0)
#define SEC(x) ((int64_t)(x)*1000000)
static unsigned checks;
static const FermentationRecipe recipe = {.name="Test", .gravityUnit=BREW_UNIT_SG,
    .confirmationSamples=3, .stepCount=3, .steps={
        {.name="A", .targetC=20, .maximumUs=5*RECIPE_DAY_US, .gravityLimit=1.02f, .hasGravityLimit=true},
        {.name="B", .targetC=12, .maximumUs=5*RECIPE_DAY_US, .gravityLimit=1.01f, .hasGravityLimit=true},
        {.name="C", .targetC=3, .maximumUs=10*RECIPE_DAY_US}}};
static FermentationRecipeProgress p;
static bool tick(RecipeGravitySample sample, int64_t now) {
    return fermentation_recipe_tick(&recipe, &p, sample, SEC(1800), now);
}
static RecipeGravitySample reading(float value, int64_t now, uint32_t sequence) {
    return (RecipeGravitySample){.valid=true, .unit=BREW_UNIT_SG, .gravity=value,
        .receivedAtUs=now, .sensorId=1, .bootId=1, .sequence=sequence};
}
int main(void) {
    CHECK(fermentation_recipe_valid(&recipe));
    fermentation_recipe_begin(&p, 0);
    CHECK(!tick((RecipeGravitySample){0}, 5*RECIPE_DAY_US-1));
    CHECK(p.stepIndex==0);
    CHECK(tick((RecipeGravitySample){0}, 5*RECIPE_DAY_US));
    CHECK(p.stepIndex==1 && p.elapsedUs==0 && p.reason==RECIPE_REASON_TIME);
    CHECK(tick((RecipeGravitySample){0}, 10*RECIPE_DAY_US));
    CHECK(p.stepIndex==2 && p.elapsedUs==0);
    CHECK(!tick((RecipeGravitySample){0}, 20*RECIPE_DAY_US-1));
    CHECK(tick((RecipeGravitySample){0}, 20*RECIPE_DAY_US));
    CHECK(p.state==RECIPE_COMPLETED && p.stepIndex==2);
    CHECK(!tick((RecipeGravitySample){0}, 50*RECIPE_DAY_US));

    // A healthy but unchanged reading above the limit never blocks the deadline.
    fermentation_recipe_begin(&p, 0);
    for (unsigned i=0; i<5; ++i) CHECK(!tick(reading(1.05f, i*RECIPE_DAY_US, i+1), i*RECIPE_DAY_US));
    CHECK(tick(reading(1.05f, 5*RECIPE_DAY_US, 6), 5*RECIPE_DAY_US));
    CHECK(p.reason==RECIPE_REASON_TIME);

    fermentation_recipe_begin(&p, 0);
    RecipeGravitySample s=reading(1.02f, SEC(1), 1);
    CHECK(!tick(s, SEC(1)) && p.confirmationCount==1);
    for (unsigned i=0; i<100; ++i) CHECK(!tick(s, SEC(2)) && p.confirmationCount==1);
    // Same message identity is not new even if a buggy sender refreshes time.
    s.receivedAtUs=SEC(3); CHECK(!tick(s, SEC(3)) && p.confirmationCount==1);
    s=reading(1.019f, SEC(4), 2); CHECK(!tick(s, SEC(4)) && p.confirmationCount==2);
    s=reading(1.021f, SEC(5), 3); CHECK(!tick(s, SEC(5)) && p.confirmationCount==0);
    s=reading(1.019f, SEC(6), 4); CHECK(!tick(s, SEC(6)));
    s=reading(1.019f, SEC(7), 5); CHECK(!tick(s, SEC(7)));
    s=reading(1.0f, SEC(8), 6); CHECK(tick(s, SEC(8)));
    CHECK(p.stepIndex==1 && p.reason==RECIPE_REASON_GRAVITY && p.elapsedUs==0);
    CHECK(!tick(s, SEC(9)) && p.confirmationCount==0); // not reused in next step

    fermentation_recipe_begin(&p, 0);
    CHECK(!tick(reading(1.0f, 5*RECIPE_DAY_US-SEC(2), 1), 5*RECIPE_DAY_US-SEC(2)));
    CHECK(!tick(reading(1.0f, 5*RECIPE_DAY_US-SEC(1), 2), 5*RECIPE_DAY_US-SEC(1)));
    CHECK(tick(reading(1.0f, 5*RECIPE_DAY_US, 3), 5*RECIPE_DAY_US));
    CHECK(p.reason==RECIPE_REASON_TIME); // deterministic tie at the deadline

    fermentation_recipe_begin(&p, 0);
    CHECK(!tick(reading(1.0f, SEC(1), 1), SEC(1)));
    s=reading(1.0f, SEC(2), 2); s.unit=BREW_UNIT_PLATO;
    CHECK(!tick(s, SEC(2)) && p.confirmationCount==0);
    s.unit=BREW_UNIT_UNKNOWN; CHECK(!tick(s, SEC(2)));
    s=reading(NAN, SEC(3), 3); CHECK(!tick(s, SEC(3)));
    s.gravity=INFINITY; CHECK(!tick(s, SEC(3)));
    s.gravity=0.5f; CHECK(!tick(s, SEC(3)));
    s=reading(1.0f, SEC(4), 4); CHECK(!tick(s, SEC(3))); // future timestamp
    s=reading(1.0f, SEC(4), 4); CHECK(!tick(s, SEC(4)));
    CHECK(!tick(s, SEC(1805)) && p.confirmationCount==0); // stale reading
    s=reading(1.0f, SEC(1806), 5); CHECK(!tick(s, SEC(1806)) && p.confirmationCount==1);
    s=reading(1.0f, SEC(1807), 6); s.sensorId=2;
    CHECK(!tick(s, SEC(1807)) && p.confirmationCount==1); // new sensor resets chain
    CHECK(tick((RecipeGravitySample){0}, 5*RECIPE_DAY_US));

    // Density noise, missing, wrong units and long tick rollovers cannot stall time.
    for (unsigned scenario=0; scenario<100; ++scenario) {
        const int64_t origin=(int64_t)scenario*100*RECIPE_DAY_US;
        fermentation_recipe_begin(&p, origin);
        for (unsigned i=0; i<5; ++i) {
            s=reading(1.05f, origin+i*RECIPE_DAY_US, i+1);
            if (scenario%3==0) s.valid=false;
            if (scenario%3==1) s.unit=BREW_UNIT_UNKNOWN;
            CHECK(!tick(s, origin+i*RECIPE_DAY_US));
        }
        CHECK(tick(s, origin+5*RECIPE_DAY_US));
        CHECK(p.stepIndex==1 && p.reason==RECIPE_REASON_TIME);
    }
    // A delayed task enters one next step, rather than skipping its temperature.
    fermentation_recipe_begin(&p, 0);
    CHECK(tick((RecipeGravitySample){0}, 30*RECIPE_DAY_US));
    CHECK(p.stepIndex==1 && p.elapsedUs==0);
    // Resume accumulates powered time, independently of RTC/wall-clock changes.
    fermentation_recipe_begin(&p, SEC(100)); p.elapsedUs=4*RECIPE_DAY_US;
    CHECK(tick((RecipeGravitySample){0}, SEC(100)+RECIPE_DAY_US));
    CHECK(p.stepIndex==1 && p.elapsedUs==0);
    puts("Recipe logic: all checks passed"); printf("%u recipe logic checks\n", checks);
}
'''

PARSER = r'''
#include "FermentationRecipeLogic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char input[8192]; size_t length=fread(input, 1, sizeof(input), stdin);
    FermentationRecipe result, original; memset(&result, 0x55, sizeof(result)); original=result;
    if (!fermentation_recipe_parse(input, length, &result)) {
        assert(!memcmp(&result, &original, sizeof(result))); puts("null"); return 0;
    }
    assert(fermentation_recipe_valid(&result));
    printf("{\"steps\":%u,\"unit\":%u,\"confirm\":%u,\"maximum_us\":%lld,\"target\":%.3f}\n",
        result.stepCount, result.gravityUnit, result.confirmationSamples,
        (long long)result.steps[0].maximumUs, (double)result.steps[0].targetC);
}
'''

MOCK = r'''
#ifndef RECIPE_MOCK_H
#define RECIPE_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef unsigned nvs_handle_t;
typedef struct MockQueue *QueueHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define NVS_READWRITE 1
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
#define configTICK_RATE_HZ 1000
#define CONFIG_BEER_ISPINDEL_STALE_SECONDS 1800
#define ESP_LOGI(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) mock_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) mock_log(tag, __VA_ARGS__)
void mock_log(const char *, const char *, ...);
const char *esp_err_to_name(esp_err_t);
int64_t esp_timer_get_time(void);
TickType_t xTaskGetTickCount(void);
QueueHandle_t xQueueCreate(unsigned, unsigned);
void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
int xQueuePeek(QueueHandle_t, void *, unsigned);
int xQueueOverwrite(QueueHandle_t, const void *);
void vTaskDelayUntil(TickType_t *, TickType_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
#endif
'''

ADAPTER = r'''
#include "FermentationRecipe.c"
#include "embedded.h"
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>

#define CHECK(x) do { assert(x); ++checks; } while (0)
#define SEC(x) ((int64_t)(x)*1000000)
struct MockQueue { unsigned size; bool full; unsigned char data[2048]; };
static struct MockQueue queues[3];
static unsigned queueCount, checks, iteration, commits;
static SavedRecipe blob, staging;
static bool haveBlob, failSave, failCommit, failTask, failQueue, failOpen;
static int64_t now;
static bool haveGravity;
static IspindelReading sample;
static float target=18;
static bool controlEnabled=true;
static void (*task)(void *);
static jmp_buf finish;
static const char *mode;
static const char *text=(const char *)embeddedRecipe;

void mock_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
int64_t esp_timer_get_time(void) { return now; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(now/1000); }
QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    CHECK(count==1 && queueCount<3 && size<=sizeof(queues[0].data));
    if (failQueue && queueCount==1) return NULL;
    QueueHandle_t q=&queues[queueCount++]; q->size=size; return q;
}
void vQueueDelete(QueueHandle_t q) { q->full=false; }
int xQueueOverwrite(QueueHandle_t q, const void *value) { memcpy(q->data,value,q->size); q->full=true; return pdTRUE; }
int xQueueSend(QueueHandle_t q, const void *value, unsigned wait) {
    CHECK(wait==0); if (q->full) return 0; return xQueueOverwrite(q,value);
}
int xQueuePeek(QueueHandle_t q, void *value, unsigned wait) {
    CHECK(wait==0); if (!q->full) return 0; memcpy(value,q->data,q->size); return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *value, unsigned wait) {
    int result=xQueuePeek(q,value,wait); q->full=false; return result;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
    CHECK(!strcmp(name,"fermentation_recipe") && stack>=6144 && priority==2 && !arg && !handle);
    if (failTask) return 0;
    task=fn; return pdPASS;
}
bool temperature_control_target_valid(float value) { return isfinite(value) && value>=-4.5f && value<=39.5f; }
bool temperature_control_set_target(float value, bool enabled) {
    CHECK(temperature_control_target_valid(value)); target=value; controlEnabled=enabled; return true;
}
bool temperature_control_latest(TemperatureControlStatus *out) {
    *out=(TemperatureControlStatus){.targetC=target,.enabled=controlEnabled}; return true;
}
bool ispindel_receiver_latest(IspindelReading *out) { if (haveGravity) *out=sample; return haveGravity; }
int nvs_open(const char *name, int openMode, nvs_handle_t *out) {
    CHECK(!strcmp(name,"fermentation") && openMode==NVS_READWRITE);
    if (failOpen) return ESP_FAIL;
    *out=1; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(handle==1); }
int nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    CHECK(handle==1 && !strcmp(key,"run") && *size==sizeof(blob));
    if (!haveBlob) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out,&blob,sizeof(blob)); return ESP_OK;
}
int nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    CHECK(handle==1 && !strcmp(key,"run") && size==sizeof(blob));
    if (failSave) return ESP_FAIL;
    memcpy(&staging,value,size); return ESP_OK;
}
int nvs_commit(nvs_handle_t handle) {
    CHECK(handle==1); if (failCommit) return ESP_FAIL;
    blob=staging; haveBlob=true; ++commits; return ESP_OK;
}
static FermentationRecipeStatus expect(FermentationRecipeState state, unsigned step, float expectedTarget) {
    FermentationRecipeStatus status; CHECK(fermentation_recipe_latest(&status));
    CHECK(status.state==state && status.stepIndex==step && status.updatedAtUs==now);
    CHECK(target==expectedTarget);
    if (state==RECIPE_RUNNING || state==RECIPE_COMPLETED) CHECK(controlEnabled);
    return status;
}
static void density(uint32_t sequence) {
    haveGravity=true;
    sample=(IspindelReading){.receivedAtUs=now, .receivedAt=xTaskGetTickCount(),
        .packet={.header={.gravityUnit=BREW_UNIT_SG,.sensorId=1,.bootId=1,.sequence=sequence},.gravity=1.019f}};
}
void vTaskDelayUntil(TickType_t *wake, TickType_t period) {
    CHECK(period==1000 && wake); *wake+=period;
    if (!strcmp(mode,"resume")) {
        switch (iteration++) {
            case 0: expect(RECIPE_RUNNING,1,12); now=RECIPE_DAY_US; break;
            case 1: expect(RECIPE_RUNNING,2,3); now+=10*RECIPE_DAY_US; break;
            case 2: expect(RECIPE_COMPLETED,2,3); longjmp(finish,1);
            default: assert(false);
        }
        return;
    }
    if (!strcmp(mode,"completed")) {
        expect(RECIPE_COMPLETED,2,3); CHECK(commits==0); longjmp(finish,1);
    }
    if (!strcmp(mode,"cancelled")) {
        CHECK(!controlEnabled); expect(RECIPE_IDLE,1,18); longjmp(finish,1);
    }
    switch (iteration++) {
        case 0: {
            FermentationRecipeStatus status=expect(RECIPE_RUNNING,0,20);
            CHECK(status.storageOk==!(failSave || failCommit));
            FermentationRecipe current; CHECK(fermentation_recipe_current(&current));
            CHECK(current.stepCount==3 && current.steps[0].targetC==20);
            CHECK(fermentation_recipe_submit_json(text,strlen(text))==ESP_ERR_INVALID_STATE);
            now=RECIPE_DAY_US; density(1); break;
        }
        case 1: CHECK(expect(RECIPE_RUNNING,0,20).confirmationCount==1); now+=SEC(1); density(2); break;
        case 2: CHECK(expect(RECIPE_RUNNING,0,20).confirmationCount==2); now+=SEC(1); density(3); break;
        case 3:
            CHECK(expect(RECIPE_RUNNING,1,12).reason==RECIPE_REASON_GRAVITY);
            // Cached density becomes stale: time alone must advance the stage.
            now+=5*RECIPE_DAY_US; break;
        case 4:
            CHECK(expect(RECIPE_RUNNING,2,3).reason==RECIPE_REASON_TIME);
            now+=10*RECIPE_DAY_US; haveGravity=false; break;
        case 5:
            expect(RECIPE_COMPLETED,2,3);
            CHECK(fermentation_recipe_cancel()==ESP_OK);
            CHECK(fermentation_recipe_submit_json(text,strlen(text))==ESP_ERR_TIMEOUT); break;
        case 6:
            expect(RECIPE_IDLE,2,3); CHECK(!controlEnabled);
            if (!failSave && !failCommit) CHECK(blob.state==RECIPE_IDLE && blob.reason==RECIPE_REASON_CANCELLED);
            longjmp(finish,1);
        default: assert(false);
    }
}
int main(int argc, char **argv) {
    CHECK(argc==2); mode=argv[1];
    CHECK(fermentation_recipe_submit_json(text,strlen(text))==ESP_ERR_INVALID_STATE);
    CHECK(fermentation_recipe_cancel()==ESP_ERR_INVALID_STATE);
    CHECK(!fermentation_recipe_latest(NULL));
    failSave=!strcmp(mode,"save-fail"); failCommit=!strcmp(mode,"commit-fail");
    failTask=!strcmp(mode,"task-fail"); failQueue=!strcmp(mode,"queue-fail"); failOpen=!strcmp(mode,"open-fail");
    if (!strcmp(mode,"resume") || !strcmp(mode,"completed") || !strcmp(mode,"cancelled") || !strcmp(mode,"corrupt")) {
        CHECK(fermentation_recipe_parse(text,strlen(text),&blob.recipe));
        blob.magic=0x52435031; blob.version=1; blob.state=RECIPE_RUNNING;
        blob.stepIndex=1; blob.elapsedUs=4*RECIPE_DAY_US; haveBlob=true;
        if (!strcmp(mode,"completed")) { blob.state=RECIPE_COMPLETED; blob.stepIndex=2; blob.elapsedUs=10*RECIPE_DAY_US; }
        if (!strcmp(mode,"cancelled")) { blob.state=RECIPE_IDLE; blob.reason=RECIPE_REASON_CANCELLED; }
        if (!strcmp(mode,"corrupt")) blob.magic=0;
    }
    esp_err_t result=fermentation_recipe_start();
    if (failTask || failQueue) {
        CHECK(result==ESP_ERR_NO_MEM && !commands && !statuses && !controlEnabled);
    } else {
        CHECK(result==ESP_OK && task);
        CHECK(fermentation_recipe_start()==ESP_ERR_INVALID_STATE);
        if (!strcmp(mode,"corrupt") || failOpen) {
            FermentationRecipeStatus status; CHECK(fermentation_recipe_latest(&status));
            CHECK(status.state==RECIPE_FAULT && !controlEnabled && !status.storageOk);
            printf("Recipe startup fault checks: %u\n", checks); return 0;
        }
        if (!strcmp(mode,"resume")) CHECK(target==12 && controlEnabled); // restored before controller starts
        if (!strcmp(mode,"normal") || failSave || failCommit) {
            CHECK(fermentation_recipe_submit_json("{}",2)==ESP_ERR_INVALID_ARG);
            const char *invalidTarget="{\"version\":1,\"name\":\"Bad\",\"gravity_unit\":\"sg\",\"steps\":["
                "{\"name\":\"A\",\"target_c\":20,\"max_days\":5},"
                "{\"name\":\"B\",\"target_c\":40,\"max_days\":5}]}";
            CHECK(fermentation_recipe_submit_json(invalidTarget,strlen(invalidTarget))==ESP_ERR_INVALID_ARG);
            CHECK(target==18); // future invalid step must not change the initial target
            CHECK(fermentation_recipe_submit_json(text,strlen(text))==ESP_OK);
            CHECK(fermentation_recipe_submit_json(text,strlen(text))==ESP_ERR_TIMEOUT);
        }
        if (!setjmp(finish)) task(NULL);
    }
    printf("%u recipe RTOS/NVS checks passed (%s)\n", checks, mode);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler or not (CJSON / "cJSON.c").exists():
        raise SystemExit("gcc and the managed cJSON dependency are required.")
    BUILD.mkdir(parents=True, exist_ok=True)
    for name in ["esp_err.h", "esp_log.h", "esp_timer.h", "sdkconfig.h", "nvs.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "recipe_mock.h"\n', encoding="utf-8")
    (BUILD / "recipe_mock.h").write_text(MOCK, encoding="utf-8")
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-DCJSON_NESTING_LIMIT=8",
              "-I" + str(BUILD), "-I" + str(PROJECT / "main"), "-I" + str(CJSON)]
    sources = [str(PROJECT / "main/FermentationRecipeLogic.c"),
               str(PROJECT / "main/FermentationRecipeJson.c"), str(CJSON / "cJSON.c")]

    def compile_test(name, content, flags=()):
        source = BUILD / (name + ".c")
        source.write_text(content, encoding="utf-8")
        executable = BUILD / name
        subprocess.run([*common, *flags, str(source), *sources, "-lm", "-o", str(executable)], check=True)
        return executable

    subprocess.run([str(compile_test("logic", LOGIC))], check=True, timeout=10)
    parser = compile_test("parser", PARSER)

    def parse(body):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode()
        return json.loads(subprocess.run([str(parser)], input=body, check=True,
            capture_output=True, timeout=5).stdout)

    example_bytes = (PROJECT / "recipes/exemplo.json").read_bytes()
    example = json.loads(example_bytes)
    assert parse(example)["maximum_us"] == 5 * 86400000000
    cases = 1
    valid = deepcopy(example)
    del valid["steps"][0]["gravity_limit"]
    assert parse(valid)
    valid["steps"][0]["max_days"] = 0.5
    assert parse(valid)["maximum_us"] == 43200000000
    valid["gravity_unit"] = "plato"
    assert parse(valid)["unit"] == 2
    cases += 3
    invalid = []
    for field in ("version", "name", "gravity_unit", "steps"):
        item = deepcopy(example); del item[field]; invalid.append(item)
    for field, values in {
        "version": [0, 2, "1", None], "name": ["", "x" * 49, 5, None],
        "gravity_unit": ["unknown", "SG", 1, None],
        "gravity_confirm_samples": [0, 11, 1.5, "3", None],
        "on_complete": ["restart", "off", 5], "steps": [[], [example["steps"][0]] * 17, {}],
    }.items():
        for value in values:
            item = deepcopy(example); item[field] = value; invalid.append(item)
    for field, values in {
        "target_c": ["20", None, -56, 126, {}, []],
        "max_days": [0, -1, 366, "5", None, 1e-10],
        "gravity_limit": ["1.020", [], {}, 0.7, 1.6],
        "name": ["", "x" * 49, None],
    }.items():
        for value in values:
            item = deepcopy(example); item["steps"][0][field] = value; invalid.append(item)
    for field in ("name", "target_c", "max_days"):
        item = deepcopy(example); del item["steps"][0][field]; invalid.append(item)
    item = deepcopy(example); item["unexpected"] = 1; invalid.append(item)
    item = deepcopy(example); item["steps"][0]["unexpected"] = 1; invalid.append(item)
    for item in invalid:
        assert parse(item) is None, item
        cases += 1
    for body in [b"", b"{}", example_bytes + b"garbage", example_bytes + b"\0", b" " * 4097,
                 b'[[[[{}]]]]', example_bytes.replace(b'"version": 1', b'"version": 1, "version": 1'),
                 example_bytes.replace(b'"target_c": 20', b'"target_c": 20, "target_c": 20'),
                 example_bytes.replace(b'Exemplo 20-12-3 C', b'Exemplo\\u0000oculto')]:
        assert parse(body) is None, body[:100]
        cases += 1
    print(f"{cases} recipe parser scenarios passed")
    embedded = 'const uint8_t embeddedRecipe[] __asm__("_binary_exemplo_json_start") = ' + json.dumps(example_bytes.decode()) + ';\n'
    (BUILD / "embedded.h").write_text(embedded, encoding="utf-8")
    adapter = compile_test("adapter", ADAPTER)
    for mode in ("normal", "resume", "completed", "cancelled", "corrupt", "save-fail",
                 "commit-fail", "queue-fail", "task-fail", "open-fail"):
        subprocess.run([str(adapter), mode], check=True, timeout=10)
    auto = compile_test("adapter_auto", ADAPTER, ["-DCONFIG_BEER_RECIPE_START_EMBEDDED=1"])
    for mode in ("auto", "resume", "completed", "cancelled", "corrupt"):
        subprocess.run([str(auto), mode], check=True, timeout=10)


if __name__ == "__main__":
    main()
