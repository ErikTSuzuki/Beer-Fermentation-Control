#include "FermentationRecipe.h"
#include "IspindelReceiver.h"
#include "TemperatureControl.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "RECIPE";
static QueueHandle_t commands, statuses, profiles;
static nvs_handle_t storage;
static bool storageOpen, storageOk;
static esp_err_t lastError;
static FermentationRecipe recipe;
static FermentationRecipeProgress progress;
static int64_t lastSaveAttemptUs;
static bool pendingSave;

typedef enum { RECIPE_COMMAND_START, RECIPE_COMMAND_CANCEL } CommandKind;
typedef struct { CommandKind kind; FermentationRecipe recipe; } RecipeCommand;
typedef struct {
    uint32_t magic, version;
    FermentationRecipe recipe;
    uint8_t state, reason, stepIndex;
    int64_t elapsedUs;
} SavedRecipe;

static bool targetsValid(const FermentationRecipe *candidate) {
    for (unsigned i = 0; i < candidate->stepCount; ++i)
        if (!temperature_control_target_valid(candidate->steps[i].targetC)) return false;
    return true;
}

static void disableControl(void) {
    TemperatureControlStatus status;
    if (temperature_control_latest(&status))
        (void)temperature_control_set_target(status.targetC, false);
}

static bool applyTarget(void) {
    if (progress.state != RECIPE_RUNNING && progress.state != RECIPE_COMPLETED) return true;
    if (temperature_control_set_target(recipe.steps[progress.stepIndex].targetC, true)) return true;
    progress.state = RECIPE_FAULT;
    lastError = ESP_ERR_INVALID_STATE;
    disableControl();
    return false;
}

static void saveProgress(int64_t nowUs) {
    lastSaveAttemptUs = nowUs;
    SavedRecipe saved = {.magic = UINT32_C(0x52435031), .version = 1,
        .recipe = recipe, .state = (uint8_t)progress.state, .reason = (uint8_t)progress.reason,
        .stepIndex = progress.stepIndex, .elapsedUs = progress.elapsedUs};
    esp_err_t result = storageOpen
        ? nvs_set_blob(storage, "run", &saved, sizeof(saved)) : ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) result = nvs_commit(storage);
    storageOk = result == ESP_OK;
    pendingSave = !storageOk;
    if (result != ESP_OK) {
        if (result != lastError) ESP_LOGE(TAG, "Falha ao salvar progresso: %s", esp_err_to_name(result));
        lastError = result;
    } else lastError = ESP_OK;
}

static void publishStatus(bool gravityAvailable, int64_t nowUs) {
    FermentationRecipeStatus status = {.state = progress.state, .reason = progress.reason,
        .stepIndex = progress.stepIndex, .stepCount = recipe.stepCount,
        .gravityUnit = recipe.gravityUnit, .confirmationCount = progress.confirmationCount,
        .gravityAvailable = gravityAvailable, .storageOk = storageOk,
        .lastError = lastError, .updatedAtUs = nowUs};
    memcpy(status.recipeName, recipe.name, sizeof(status.recipeName));
    if (recipe.stepCount && progress.stepIndex < recipe.stepCount) {
        const FermentationStep *step = &recipe.steps[progress.stepIndex];
        memcpy(status.stepName, step->name, sizeof(status.stepName));
        status.targetC = step->targetC;
        status.elapsedDays = (double)progress.elapsedUs / RECIPE_DAY_US;
        status.maximumDays = (double)step->maximumUs / RECIPE_DAY_US;
        status.hasGravityLimit = step->hasGravityLimit;
        status.gravityLimit = step->gravityLimit;
    }
    xQueueOverwrite(statuses, &status);
}

static void beginRecipe(const FermentationRecipe *candidate, int64_t nowUs) {
    recipe = *candidate;
    xQueueOverwrite(profiles, &recipe);
    fermentation_recipe_begin(&progress, nowUs);
    saveProgress(nowUs);
    (void)applyTarget();
    ESP_LOGI(TAG, "Receita '%s' iniciada: etapa 1/%u '%s', alvo=%.2f C",
             recipe.name, recipe.stepCount, recipe.steps[0].name, (double)recipe.steps[0].targetC);
}

static RecipeGravitySample gravitySample(void) {
    IspindelReading reading = {0};
    const bool have = ispindel_receiver_latest(&reading);
    return (RecipeGravitySample){.valid = have,
        .unit = reading.packet.header.gravityUnit, .gravity = reading.packet.gravity,
        .receivedAtUs = reading.receivedAtUs, .sensorId = reading.packet.header.sensorId,
        .bootId = reading.packet.header.bootId, .sequence = reading.packet.header.sequence};
}

static void recipeTask(void *context) {
    (void)context;
    RecipeCommand command;
    TickType_t lastWake = xTaskGetTickCount();
    const int64_t maxAge = (int64_t)CONFIG_BEER_ISPINDEL_STALE_SECONDS * 1000000;
    for (;;) {
        int64_t nowUs = esp_timer_get_time();
        if (xQueueReceive(commands, &command, 0) == pdTRUE) {
            if (command.kind == RECIPE_COMMAND_CANCEL) {
                // Cancela mesmo se o armazenamento estiver falhando.
                progress.state = RECIPE_IDLE;
                progress.reason = RECIPE_REASON_CANCELLED;
                disableControl();
                saveProgress(nowUs);
                ESP_LOGI(TAG, "Receita cancelada; controle termico desativado");
            } else if (progress.state != RECIPE_RUNNING) {
                beginRecipe(&command.recipe, nowUs);
            } else {
                lastError = ESP_ERR_INVALID_STATE;
                ESP_LOGW(TAG, "Receita em andamento: cancele antes de iniciar outra");
            }
        }

        const RecipeGravitySample sample = gravitySample();
        nowUs = esp_timer_get_time();
        const bool gravityAvailable = sample.valid && sample.unit == recipe.gravityUnit &&
            fermentation_recipe_gravity_valid(sample.unit, sample.gravity) &&
            sample.receivedAtUs >= 0 && sample.receivedAtUs <= nowUs &&
            nowUs - sample.receivedAtUs <= maxAge;
        if (fermentation_recipe_tick(&recipe, &progress, sample, maxAge, nowUs)) {
            (void)applyTarget();
            saveProgress(nowUs);
            ESP_LOGI(TAG, "Etapa %u/%u '%s' | alvo=%.2f C | motivo=%s | %s",
                     progress.stepIndex + 1, recipe.stepCount, recipe.steps[progress.stepIndex].name,
                     (double)recipe.steps[progress.stepIndex].targetC,
                     progress.reason == RECIPE_REASON_GRAVITY ? "DENSIDADE" : "TEMPO",
                     progress.state == RECIPE_COMPLETED ? "CONCLUIDA; MANTENDO ALVO" : "EM EXECUCAO");
        } else if (((progress.state == RECIPE_RUNNING) &&
            nowUs - lastSaveAttemptUs >= INT64_C(900000000)) ||
            (pendingSave && nowUs - lastSaveAttemptUs >= INT64_C(60000000))) {
            saveProgress(nowUs);
        }
        publishStatus(gravityAvailable, nowUs);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
    }
}

static esp_err_t restoreProgress(void) {
    SavedRecipe *saved = calloc(1, sizeof(*saved));
    if (!saved) return ESP_ERR_NO_MEM;
    size_t size = sizeof(*saved);
    esp_err_t result = nvs_get_blob(storage, "run", saved, &size);
    if (result == ESP_OK) {
        bool valid = size == sizeof(*saved) && saved->magic == UINT32_C(0x52435031) &&
            saved->version == 1 && saved->reason <= RECIPE_REASON_CANCELLED;
        if (saved->state != RECIPE_IDLE) {
            valid = valid && (saved->state == RECIPE_RUNNING || saved->state == RECIPE_COMPLETED) &&
                fermentation_recipe_valid(&saved->recipe) && targetsValid(&saved->recipe) &&
                saved->stepIndex < saved->recipe.stepCount && saved->elapsedUs >= 0 &&
                saved->elapsedUs <= saved->recipe.steps[saved->stepIndex].maximumUs;
        }
        if (valid) {
            recipe = saved->recipe;
            progress = (FermentationRecipeProgress){.state = (FermentationRecipeState)saved->state,
                .reason = (FermentationRecipeReason)saved->reason, .stepIndex = saved->stepIndex,
                .elapsedUs = saved->elapsedUs, .lastTickUs = esp_timer_get_time(),
                .enteredAtUs = esp_timer_get_time()};
            if (progress.state == RECIPE_IDLE) {
                // Cancelamento persistido nao rearma o controle ao reiniciar.
                if (!fermentation_recipe_valid(&recipe) || !targetsValid(&recipe))
                    memset(&recipe, 0, sizeof(recipe));
                disableControl();
            } else (void)applyTarget();
        } else result = ESP_ERR_INVALID_STATE;
    }
    free(saved);
    return result;
}

esp_err_t fermentation_recipe_start(void) {
    if (commands || statuses || profiles) return ESP_ERR_INVALID_STATE;
    commands = xQueueCreate(1, sizeof(RecipeCommand));
    statuses = commands ? xQueueCreate(1, sizeof(FermentationRecipeStatus)) : NULL;
    profiles = statuses ? xQueueCreate(1, sizeof(FermentationRecipe)) : NULL;
    if (!commands || !statuses || !profiles) goto noMemory;
    esp_err_t result = nvs_open("fermentation", NVS_READWRITE, &storage);
    storageOpen = result == ESP_OK;
    if (storageOpen) result = restoreProgress();
    storageOk = result == ESP_OK || (storageOpen && result == ESP_ERR_NVS_NOT_FOUND);
    if (storageOpen && result == ESP_ERR_NVS_NOT_FOUND) {
#if CONFIG_BEER_RECIPE_START_EMBEDDED
        extern const uint8_t exampleStart[] __asm__("_binary_exemplo_json_start");
        if (fermentation_recipe_parse((const char *)exampleStart, strlen((const char *)exampleStart), &recipe) &&
            targetsValid(&recipe)) beginRecipe(&recipe, esp_timer_get_time());
        else { result = ESP_ERR_INVALID_ARG; storageOk = false; }
#endif
    }
    if (!storageOk) {
        progress.state = RECIPE_FAULT;
        lastError = result;
        disableControl();
        ESP_LOGE(TAG, "Receita indisponivel: %s; controle desativado", esp_err_to_name(result));
    }
    lastSaveAttemptUs = esp_timer_get_time();
    if (fermentation_recipe_valid(&recipe)) xQueueOverwrite(profiles, &recipe);
    publishStatus(false, lastSaveAttemptUs);
    if (xTaskCreate(recipeTask, "fermentation_recipe", 8192, NULL, 2, NULL) == pdPASS) return ESP_OK;
    if (storageOpen) { nvs_close(storage); storageOpen = false; }
noMemory:
    if (commands) vQueueDelete(commands);
    if (statuses) vQueueDelete(statuses);
    if (profiles) vQueueDelete(profiles);
    commands = statuses = profiles = NULL;
    disableControl();
    return ESP_ERR_NO_MEM;
}

esp_err_t fermentation_recipe_submit_json(const char *json, size_t length) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    RecipeCommand *command = calloc(1, sizeof(*command));
    if (!command) return ESP_ERR_NO_MEM;
    if (!fermentation_recipe_parse(json, length, &command->recipe) || !targetsValid(&command->recipe)) {
        free(command);
        return ESP_ERR_INVALID_ARG;
    }
    FermentationRecipeStatus status;
    if (!fermentation_recipe_latest(&status) || status.state == RECIPE_RUNNING) {
        free(command);
        return ESP_ERR_INVALID_STATE;
    }
    command->kind = RECIPE_COMMAND_START;
    const bool queued = xQueueSend(commands, command, 0) == pdTRUE;
    free(command);
    return queued ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t fermentation_recipe_cancel(void) {
    if (!commands) return ESP_ERR_INVALID_STATE;
    RecipeCommand *command = calloc(1, sizeof(*command));
    if (!command) return ESP_ERR_NO_MEM;
    command->kind = RECIPE_COMMAND_CANCEL;
    const bool queued = xQueueSend(commands, command, 0) == pdTRUE;
    free(command);
    return queued ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool fermentation_recipe_latest(FermentationRecipeStatus *status) {
    return status && statuses && xQueuePeek(statuses, status, 0) == pdTRUE;
}

bool fermentation_recipe_current(FermentationRecipe *current) {
    return current && profiles && xQueuePeek(profiles, current, 0) == pdTRUE;
}
