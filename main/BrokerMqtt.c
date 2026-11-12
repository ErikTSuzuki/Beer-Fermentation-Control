#include "BrokerMqtt.h"
#include "sdkconfig.h"

#if CONFIG_BEER_LOCAL_MQTT
#include "FermentationRecipe.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "TemperatureControl.h"
#include "WifiStation.h"
#include "cJSON.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "mqtt_client.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TOPIC_BASE "cervejaria/v1/fermentadores/" CONFIG_BEER_MQTT_DEVICE
#define TELEMETRY_TOPIC TOPIC_BASE "/telemetria"
#define COMMAND_TOPIC TOPIC_BASE "/comandos/receita"
#define STATE_TOPIC TOPIC_BASE "/estado/receita"
#define COMMAND_BODY_MAX (RECIPE_JSON_MAX + 256)
#define COMMAND_MAX_AGE_SECONDS 12
#define STATE_INTERVAL_MS 3000

static const char *TAG = "BROKER_MQTT";
// Odd generation numbers identify connected sessions. Only the MQTT callback writes.
static atomic_uint connection;
static atomic_uint readyConnection;
static QueueHandle_t receivedCommands;
static int commandSubscription;
static char *commandBody;
static size_t commandLength, commandReceived;

typedef struct {
    char *body;
    size_t length;
    unsigned connection;
} IncomingCommand;

typedef struct {
    uint32_t ticket;
    char id[37];
    bool cancel;
    int64_t resultDeadlineUs;
} PendingCommand;

static void resetCommandBody(void) {
    free(commandBody);
    commandBody = NULL;
    commandLength = commandReceived = 0;
}

static bool uniqueFields(const cJSON *object) {
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    return true;
}

// Limita a profundidade antes do parser e impede strings truncadas por NUL.
static bool commandStructureSafe(const char *body, size_t length) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        const char ch = body[i];
        if (!ch) return false;
        if (quoted) {
            if (escaped) {
                if (ch == 'u' && i + 4 < length && !memcmp(body + i + 1, "0000", 4)) return false;
                escaped = false;
            } else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 4) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    return !depth && !quoted;
}

static bool addTimestamp(cJSON *root) {
    struct timeval clock;
    if (gettimeofday(&clock, NULL) != 0 || clock.tv_sec < 1704067200) return true;
    struct tm utc;
    char seconds[24], timestamp[32];
    if (!gmtime_r(&clock.tv_sec, &utc) ||
        !strftime(seconds, sizeof(seconds), "%Y-%m-%dT%H:%M:%S", &utc)) return true;
    snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", seconds, (unsigned)(clock.tv_usec / 1000));
    return cJSON_AddStringToObject(root, "timestamp", timestamp) != NULL;
}

// Usa as mesmas caixas postais da tela, sem retirar dados das filas.
// Nao publica amostras invalidas ou antigas como se fossem atuais.
static int buildPayload(char *payload, size_t capacity) {
    LocalTemperature local = {0};
    IspindelReading remote = {0};
    TemperatureControlStatus control = {0};
    FermentationRecipeStatus recipe = {0};
    const bool haveLocal = local_temperature_latest(&local);
    const bool haveRemote = ispindel_receiver_latest(&remote);
    const bool haveControl = temperature_control_latest(&control);
    const bool haveRecipe = fermentation_recipe_latest(&recipe);
    const int64_t nowUs = esp_timer_get_time();
    const TickType_t now = xTaskGetTickCount();
    const bool localOk = haveLocal && local.valid && isfinite(local.temperatureC) &&
        now - local.sampledAt <= pdMS_TO_TICKS(5000);
    const BrewTelemetry *p = &remote.packet;
    const bool remoteOk = haveRemote && ispindel_reading_is_current(&remote, nowUs) &&
        isfinite(p->temperatureC) && isfinite(p->tiltDegrees) && isfinite(p->gravity) && isfinite(p->batteryV);
    const bool controlOk = haveControl && nowUs >= control.updatedAtUs &&
        nowUs - control.updatedAtUs <= INT64_C(2000000);
    const bool recipeOk = haveRecipe && nowUs >= recipe.updatedAtUs &&
        nowUs - recipe.updatedAtUs <= INT64_C(2500000);
    if (!localOk && !remoteOk && !controlOk && !recipeOk) return 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    if (!addTimestamp(root)) goto failure;
    int fields = 0;
    if (localOk) {
        if (!cJSON_AddNumberToObject(root, "temperatura-local", round(local.temperatureC * 1000.0) / 1000.0)) goto failure;
        ++fields;
    }
    if (remoteOk) {
        const char *unit = p->header.gravityUnit == BREW_UNIT_SG ? "sg" :
            p->header.gravityUnit == BREW_UNIT_PLATO ? "plato" : "unknown";
        char sensorId[16];
        snprintf(sensorId, sizeof(sensorId), "%06" PRIX32, p->header.sensorId);
        cJSON *gravity = cJSON_AddObjectToObject(root, "densidade");
        cJSON *context = gravity ? cJSON_AddObjectToObject(gravity, "context") : NULL;
        if (!context || !cJSON_AddNumberToObject(gravity, "value", round(p->gravity * 1000000.0) / 1000000.0) ||
            !cJSON_AddStringToObject(context, "unit", unit) || !cJSON_AddStringToObject(context, "sensor_id", sensorId) ||
            !cJSON_AddNumberToObject(root, "temperatura-ispindel", round(p->temperatureC * 1000.0) / 1000.0) ||
            !cJSON_AddNumberToObject(root, "inclinacao", round(p->tiltDegrees * 1000.0) / 1000.0) ||
            !cJSON_AddNumberToObject(root, "bateria", round(p->batteryV * 1000.0) / 1000.0)) goto failure;
        fields += 4;
    }
    if (controlOk) {
        cJSON *state = cJSON_AddObjectToObject(root, "estado-controle");
        cJSON *context = state ? cJSON_AddObjectToObject(state, "context") : NULL;
        if (!context || !cJSON_AddNumberToObject(root, "temperatura-alvo", control.targetC) ||
            !cJSON_AddNumberToObject(state, "value", control.state) ||
            !cJSON_AddStringToObject(context, "estado", temperature_control_state_name(control.state)) ||
            !cJSON_AddBoolToObject(context, "habilitado", control.enabled) ||
            !cJSON_AddBoolToObject(context, "sensor_valido", control.sensorValid) ||
            !cJSON_AddBoolToObject(context, "aquecedor", control.heaterOn) ||
            !cJSON_AddBoolToObject(context, "refrigerador", control.coolerOn) ||
            !cJSON_AddNumberToObject(context, "espera_s", control.waitSeconds)) goto failure;
        fields += 2;
    }
    if (recipeOk) {
        cJSON *stage = cJSON_AddObjectToObject(root, "etapa-fermentacao");
        cJSON *context = stage ? cJSON_AddObjectToObject(stage, "context") : NULL;
        const unsigned index = recipe.state == RECIPE_IDLE || !recipe.stepCount ? 0 : recipe.stepIndex + 1;
        if (!context || !cJSON_AddNumberToObject(stage, "value", index) ||
            !cJSON_AddNumberToObject(context, "estado", recipe.state) ||
            !cJSON_AddStringToObject(context, "receita", recipe.recipeName) ||
            !cJSON_AddStringToObject(context, "etapa", recipe.stepName) ||
            !cJSON_AddNumberToObject(context, "total_etapas", recipe.stepCount) ||
            !cJSON_AddNumberToObject(context, "motivo", recipe.reason) ||
            !cJSON_AddBoolToObject(context, "densidade_disponivel", recipe.gravityAvailable) ||
            !cJSON_AddBoolToObject(context, "armazenamento_ok", recipe.storageOk) ||
            !cJSON_AddNumberToObject(context, "erro", recipe.lastError) ||
            !cJSON_AddNumberToObject(root, "dias-etapa", round(recipe.elapsedDays * 100000.0) / 100000.0) ||
            !cJSON_AddNumberToObject(root, "dias-max-etapa", recipe.maximumDays)) goto failure;
        if (recipe.hasGravityLimit && !cJSON_AddNumberToObject(context, "densidade_limite", recipe.gravityLimit)) goto failure;
        fields += 3;
    }
    // Escrita limitada: inclui terminador e rejeita payload truncado.
    char *json = cJSON_PrintUnformatted(root);
    if (!json) goto failure;
    const size_t length = strlen(json);
    if (length + 1 > capacity) { cJSON_free(json); goto failure; }
    memcpy(payload, json, length + 1);
    cJSON_free(json);
    cJSON_Delete(root);
    return fields;
failure:
    cJSON_Delete(root);
    return -1;
}

static void receiveCommand(esp_mqtt_event_handle_t event) {
    const unsigned generation = atomic_load(&connection);
    if (!event || !(generation & 1) || atomic_load(&readyConnection) != generation) return;
    if (event->current_data_offset == 0) {
        resetCommandBody();
        if (event->retain || !event->topic || event->topic_len != sizeof(COMMAND_TOPIC) - 1 ||
            memcmp(event->topic, COMMAND_TOPIC, sizeof(COMMAND_TOPIC) - 1) ||
            event->total_data_len <= 0 || event->total_data_len > COMMAND_BODY_MAX) return;
        commandLength = (size_t)event->total_data_len;
        commandBody = malloc(commandLength + 1);
    }
    if (!commandBody) return;
    if (event->retain || event->data_len <= 0 || !event->data ||
        event->current_data_offset < 0 || (size_t)event->current_data_offset != commandReceived ||
        event->total_data_len != (int)commandLength ||
        (size_t)event->data_len > commandLength - commandReceived) {
        resetCommandBody();
        return;
    }
    memcpy(commandBody + commandReceived, event->data, (size_t)event->data_len);
    commandReceived += (size_t)event->data_len;
    if (commandReceived == commandLength) {
        commandBody[commandLength] = '\0';
        const IncomingCommand incoming = {commandBody, commandLength, generation};
        if (xQueueSend(receivedCommands, &incoming, 0) == pdTRUE) commandBody = NULL;
        resetCommandBody();
    }
}

static void onMqttEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t event = data;
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            resetCommandBody();
            atomic_store(&readyConnection, 0);
            atomic_store(&connection, (atomic_load(&connection) + 2) | 1);
            commandSubscription = esp_mqtt_client_subscribe(event->client, COMMAND_TOPIC, 1);
            ESP_LOGI(TAG, "MQTT conectado. Telemetria em %s", TELEMETRY_TOPIC);
            if (commandSubscription < 0) ESP_LOGW(TAG, "Falha ao assinar %s", COMMAND_TOPIC);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            if (event->msg_id != commandSubscription) break;
            if (event->data_len > 0 && event->data && (unsigned char)event->data[0] >= 0x80) {
                ESP_LOGW(TAG, "Broker recusou a assinatura de %s", COMMAND_TOPIC);
                break;
            }
            atomic_store(&readyConnection, atomic_load(&connection));
            ESP_LOGI(TAG, "Assinatura confirmada: %s", COMMAND_TOPIC);
            break;
        case MQTT_EVENT_DATA:
            receiveCommand(event);
            break;
        case MQTT_EVENT_DISCONNECTED:
            atomic_store(&connection, atomic_load(&connection) & ~1U);
            atomic_store(&readyConnection, 0);
            resetCommandBody();
            ESP_LOGW(TAG, "MQTT desconectado");
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "Falha MQTT. Confira o endereco do broker e a credencial do ESP.");
            break;
        default:
            break;
    }
}

static void publishResult(esp_mqtt_client_handle_t client, const char *id, esp_err_t error, bool stored) {
    char payload[192];
    const char *outcome = error == ESP_OK ? "applied" : "rejected";
    const char *code = error == ESP_OK ? "ok" :
        error == ESP_ERR_INVALID_ARG ? "invalid_recipe" :
        error == ESP_ERR_INVALID_STATE ? "state_changed" :
        error == ESP_ERR_TIMEOUT || error == ESP_ERR_NO_MEM ? "busy" : "control_error";
    snprintf(payload, sizeof(payload),
        "{\"kind\":\"result\",\"id\":\"%s\",\"outcome\":\"%s\",\"code\":\"%s\",\"storage_ok\":%s}",
        id, outcome, code, stored ? "true" : "false");
    if (esp_mqtt_client_enqueue(client, STATE_TOPIC, payload, 0, 1, 0, false) < 0)
        ESP_LOGW(TAG, "Falha ao enviar confirmacao da receita");
}

static bool validRequestId(const char *id) {
    if (strlen(id) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static bool uint32Field(const cJSON *field) {
    return cJSON_IsNumber(field) && field->valuedouble >= 0 &&
        field->valuedouble <= UINT32_MAX && floor(field->valuedouble) == field->valuedouble;
}

static void dispatchCommand(esp_mqtt_client_handle_t client, const IncomingCommand *command,
                            const char *session, PendingCommand *pending) {
    static char seen[32][37];
    static unsigned nextSeen;
    static uint32_t lastTicket;
    if (memchr(command->body, 0, command->length) ||
        !commandStructureSafe(command->body, command->length)) return;
    cJSON *root = cJSON_ParseWithLengthOpts(command->body, command->length + 1, NULL, true);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "session");
    const cJSON *lease = cJSON_GetObjectItemCaseSensitive(root, "lease");
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "recipe");
    const uint32_t nowSeconds = esp_timer_get_time() / 1000000;
    if (!uniqueFields(root) || !cJSON_IsString(id) || !validRequestId(id->valuestring) ||
        !cJSON_IsString(token) || strcmp(token->valuestring, session) ||
        !uint32Field(lease) || lease->valuedouble > nowSeconds ||
        nowSeconds - (uint32_t)lease->valuedouble > COMMAND_MAX_AGE_SECONDS ||
        !uint32Field(revision) || !cJSON_IsString(action)) goto cleanup;
    for (size_t i = 0; i < sizeof(seen) / sizeof(seen[0]); ++i) {
        if (!strcmp(seen[i], id->valuestring)) goto cleanup;
    }
    strcpy(seen[nextSeen], id->valuestring);
    nextSeen = (nextSeen + 1) % (sizeof(seen) / sizeof(seen[0]));
    const bool cancel = !strcmp(action->valuestring, "cancel");
    const bool start = !strcmp(action->valuestring, "start");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (pending->ticket) {
        result = ESP_ERR_TIMEOUT;
    } else if ((cancel && !profile) || (start && cJSON_IsObject(profile))) {
        char *recipe = start ? cJSON_PrintUnformatted(profile) : NULL;
        if (++lastTicket == 0) ++lastTicket;
        if (atomic_load(&readyConnection) != command->connection) {
            cJSON_free(recipe);
            goto cleanup;
        }
        const int64_t expiresAtUs = ((int64_t)lease->valuedouble + COMMAND_MAX_AGE_SECONDS + 1) * 1000000;
        result = start && !recipe ? ESP_ERR_NO_MEM : fermentation_recipe_submit_remote(
            recipe, recipe ? strlen(recipe) : 0, (uint32_t)revision->valuedouble, lastTicket, expiresAtUs);
        cJSON_free(recipe);
        if (result == ESP_OK) {
            *pending = (PendingCommand){.ticket = lastTicket, .cancel = cancel,
                .resultDeadlineUs = esp_timer_get_time() + INT64_C(6000000)};
            strcpy(pending->id, id->valuestring);
        }
    }
    if (result != ESP_OK) publishResult(client, id->valuestring, result, false);
cleanup:
    cJSON_Delete(root);
}

static bool publishState(esp_mqtt_client_handle_t client, const char *session) {
    FermentationRecipeStatus run, verified;
    TemperatureControlStatus control;
    TemperatureControlSettings settings;
    if (!fermentation_recipe_latest(&run) || !temperature_control_latest(&control)) return false;
    temperature_control_get_settings(&settings);
    FermentationRecipe *profile = malloc(sizeof(*profile));
    if (!profile) return false;
    cJSON *recipe = fermentation_recipe_current(profile) ? fermentation_recipe_json(profile) : cJSON_CreateNull();
    free(profile);
    if (!recipe || !fermentation_recipe_latest(&verified) || verified.revision != run.revision) {
        cJSON_Delete(recipe);
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddItemToObject(root, "recipe", recipe)) {
        cJSON_Delete(recipe);
        cJSON_Delete(root);
        return false;
    }
    const bool complete = cJSON_AddStringToObject(root, "kind", "state") &&
        cJSON_AddStringToObject(root, "session", session) &&
        cJSON_AddNumberToObject(root, "lease", (uint32_t)(esp_timer_get_time() / 1000000)) &&
        cJSON_AddNumberToObject(root, "revision", run.revision) &&
        cJSON_AddNumberToObject(root, "state", run.state) &&
        cJSON_AddBoolToObject(root, "storage_ok", run.storageOk) &&
        cJSON_AddBoolToObject(root, "control_enabled", control.enabled) &&
        cJSON_AddNumberToObject(root, "target_min_c", settings.minimumC + settings.heatingDifferentialC) &&
        cJSON_AddNumberToObject(root, "target_max_c", settings.maximumC - settings.coolingDifferentialC);
    char *payload = complete ? cJSON_PrintUnformatted(root) : NULL;
    const bool sent = payload && esp_mqtt_client_publish(client, STATE_TOPIC, payload, 0, 0, 0) >= 0;
    cJSON_free(payload);
    cJSON_Delete(root);
    return sent;
}

static bool validLabel(const char *label) {
    const size_t length = strlen(label);
    if (!length || length > 40 || label[0] == '-') return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = label[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static void brokerTask(void *context) {
    (void)context;
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter MAC para o cliente MQTT");
        vTaskDelete(NULL);
        return;
    }
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "beer-monitor-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = CONFIG_BEER_MQTT_URI,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .broker.verification.crt_bundle_attach = (!strncmp(CONFIG_BEER_MQTT_URI, "mqtts://", 8) ||
            !strncmp(CONFIG_BEER_MQTT_URI, "wss://", 6))
            ? esp_crt_bundle_attach : NULL,
#endif
        .credentials.client_id = clientId,
        .credentials.username = CONFIG_BEER_MQTT_DEVICE,
        .credentials.authentication.password = CONFIG_BEER_MQTT_PASSWORD,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 15,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 3000,
        .buffer.size = 2048,
        .task.priority = 2,
        .task.stack_size = 6144,
        .outbox.limit = 16384,
    };
    while (!wifi_station_is_connected()) vTaskDelay(pdMS_TO_TICKS(1000));
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (!client || esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, onMqttEvent, NULL) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        if (client) esp_mqtt_client_destroy(client);
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT");
        vTaskDelete(NULL);
        return;
    }

    char session[33] = "";
    PendingCommand pending = {0};
    FermentationRecipeResult result = {0};
    unsigned generation = 0;
    TickType_t lastState = 0, lastTelemetry = 0;
    bool publishSoon = true, telemetrySoon = true;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_BEER_MQTT_INTERVAL_SECONDS * 1000UL);
    for (;;) {
        const unsigned current = atomic_load(&connection);
        if (current != generation) {
            generation = current;
            uint8_t random[16];
            esp_fill_random(random, sizeof(random));
            for (size_t i = 0; i < sizeof(random); ++i) snprintf(session + 2 * i, 3, "%02x", random[i]);
            publishSoon = telemetrySoon = true;
        }
        IncomingCommand command;
        if (xQueueReceive(receivedCommands, &command, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (command.connection == generation && atomic_load(&readyConnection) == generation)
                dispatchCommand(client, &command, session, &pending);
            free(command.body);
        }
        if (pending.ticket) {
            FermentationRecipeResult received;
            if (fermentation_recipe_remote_result(&received) && received.ticket == pending.ticket)
                result = received;
            // Cancellation is confirmed only after the thermal task reports both outputs off.
            TemperatureControlStatus control;
            const bool outputsOff = temperature_control_latest(&control) && !control.enabled &&
                !control.heaterOn && !control.coolerOn;
            if (result.ticket && (result.error != ESP_OK || !pending.cancel || outputsOff)) {
                if (atomic_load(&connection) & 1)
                    publishResult(client, pending.id, result.error, result.storageOk);
                pending.ticket = result.ticket = 0;
                publishSoon = telemetrySoon = true;
            } else if (esp_timer_get_time() > pending.resultDeadlineUs) {
                pending.ticket = result.ticket = 0;
                publishSoon = telemetrySoon = true;
            }
        }
        if (!(generation & 1) || atomic_load(&connection) != generation) continue;
        const TickType_t now = xTaskGetTickCount();
        if (atomic_load(&readyConnection) == generation &&
            (publishSoon || now - lastState >= pdMS_TO_TICKS(STATE_INTERVAL_MS))) {
            (void)publishState(client, session);
            lastState = now;
            publishSoon = false;
        }
        if (telemetrySoon || now - lastTelemetry >= interval) {
            char payload[2048];
            if (buildPayload(payload, sizeof(payload)) > 0 &&
                esp_mqtt_client_enqueue(client, TELEMETRY_TOPIC, payload, 0, 1, 0, false) < 0)
                ESP_LOGW(TAG, "Falha ao enfileirar telemetria");
            lastTelemetry = now;
            telemetrySoon = false;
        }
    }
}
#endif

void broker_mqtt_start(void) {
#if CONFIG_BEER_LOCAL_MQTT
    if (!CONFIG_BEER_WIFI_SSID[0] || !CONFIG_BEER_MQTT_URI[0] || !CONFIG_BEER_MQTT_PASSWORD[0]) {
        ESP_LOGW(TAG, "Configure Wi-Fi externo, URI e senha MQTT no menuconfig");
        return;
    }
    if (!validLabel(CONFIG_BEER_MQTT_DEVICE)) {
        ESP_LOGE(TAG, "Identificador MQTT invalido");
        return;
    }
    receivedCommands = xQueueCreate(1, sizeof(IncomingCommand));
    if (!receivedCommands || xTaskCreate(brokerTask, "broker_mqtt", 8192, NULL, 1, NULL) != pdPASS) {
        if (receivedCommands) vQueueDelete(receivedCommands);
        ESP_LOGE(TAG, "Falha ao criar tarefa MQTT");
    }
#endif
}
