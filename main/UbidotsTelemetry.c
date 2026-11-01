#include "UbidotsTelemetry.h"
#include "sdkconfig.h"

#if CONFIG_BEER_UBIDOTS_ENABLED
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "WifiStation.h"
#include "TemperatureControl.h"
#include "FermentationRecipe.h"
#include "cJSON.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "UBIDOTS";
static EventGroupHandle_t mqttEvents;
#define MQTT_CONNECTED_BIT BIT0

static bool validLabel(const char *label) {
    const size_t length = strlen(label);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = label[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return false;
    }
    return true;
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
    const bool remoteOk = haveRemote && now - remote.receivedAt <= ISPINDEL_STALE_AFTER_TICKS &&
        isfinite(p->temperatureC) && isfinite(p->tiltDegrees) && isfinite(p->gravity) && isfinite(p->batteryV);
    const bool controlOk = haveControl && nowUs >= control.updatedAtUs &&
        nowUs - control.updatedAtUs <= INT64_C(2000000);
    const bool recipeOk = haveRecipe && nowUs >= recipe.updatedAtUs &&
        nowUs - recipe.updatedAtUs <= INT64_C(2500000);
    if (!localOk && !remoteOk && !controlOk && !recipeOk) return 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    int dots = 0;
    if (localOk) {
        if (!cJSON_AddNumberToObject(root, "temperatura-local", round(local.temperatureC * 1000.0) / 1000.0)) goto failure;
        ++dots;
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
        dots += 4;
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
        dots += 2;
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
        dots += 3;
    }
    // Escrita limitada: inclui terminador e rejeita payload truncado.
    char *json = cJSON_PrintUnformatted(root);
    if (!json) goto failure;
    const size_t length = strlen(json);
    if (length + 1 > capacity) { cJSON_free(json); goto failure; }
    memcpy(payload, json, length + 1);
    cJSON_free(json);
    cJSON_Delete(root);
    return dots;
failure:
    cJSON_Delete(root);
    return -1;
}

static void onMqttEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t event = data;
    switch (id) {
        case MQTT_EVENT_CONNECTED:
            xEventGroupSetBits(mqttEvents, MQTT_CONNECTED_BIT);
            ESP_LOGI(TAG, "MQTT conectado ao Ubidots");
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(mqttEvents, MQTT_CONNECTED_BIT);
            ESP_LOGW(TAG, "MQTT desconectado; reconexao automatica em 30 s");
            break;
        case MQTT_EVENT_ERROR:
            xEventGroupClearBits(mqttEvents, MQTT_CONNECTED_BIT);
            if (event && event->error_handle &&
                event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "Broker recusou conexao (codigo %d); confira token/permissoes",
                         event->error_handle->connect_return_code);
            } else {
                ESP_LOGW(TAG, "Erro de transporte MQTT; confira Wi-Fi e porta TCP 1883");
            }
            break;
        default:
            break;
    }
}

static void telemetryTask(void *context) {
    (void)context;
    char topic[96];
    snprintf(topic, sizeof(topic), "/v1.6/devices/%s",
             CONFIG_BEER_UBIDOTS_DEVICE_LABEL);
    uint8_t mac[6];
    esp_err_t result = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao obter MAC para client ID: %s", esp_err_to_name(result));
        vTaskDelete(NULL);
        return;
    }
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "beer-monitor-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_mqtt_client_config_t config = {
        .broker.address.uri = "mqtt://industrial.api.ubidots.com:1883",
        .credentials.client_id = clientId,
        .credentials.username = CONFIG_BEER_UBIDOTS_TOKEN,
        .credentials.authentication.password = "",
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 15,
        .network.reconnect_timeout_ms = 30000,
        .network.timeout_ms = 5000,
        .buffer.size = 2048,
        .task.priority = 2,
        .task.stack_size = 6144,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Falha ao criar cliente MQTT");
        vTaskDelete(NULL);
        return;
    }
    result = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, onMqttEvent, NULL);
    // Inicia apenas com IP; depois o cliente gerencia as reconexoes.
    while (result == ESP_OK && !wifi_station_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (result == ESP_OK) result = esp_mqtt_client_start(client);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar MQTT: %s", esp_err_to_name(result));
        esp_mqtt_client_destroy(client);
        vTaskDelete(NULL);
        return;
    }
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_BEER_UBIDOTS_INTERVAL_SECONDS * 1000UL);
    TickType_t lastAttempt = 0;
    bool attempted = false;

    ESP_LOGI(TAG, "MQTT topico=%s; intervalo=%d s; QoS 0, retain=false",
             topic, CONFIG_BEER_UBIDOTS_INTERVAL_SECONDS);
    for (;;) {
        if (!wifi_station_is_connected() ||
            !(xEventGroupGetBits(mqttEvents) & MQTT_CONNECTED_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!attempted || xTaskGetTickCount() - lastAttempt >= interval) {
            char payload[2048];
            const int dots = buildPayload(payload, sizeof(payload));
            if (dots > 0) {
                // Intervalo medido entre inicios, inclusive apos erro ou reconexao.
                lastAttempt = xTaskGetTickCount();
                attempted = true;
                // Igual ao exemplo Arduino: QoS 0, sem retain ou fila offline.
                // O retorno confirma o transporte local, nao o armazenamento no Ubidots.
                const int messageId = esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
                if (messageId >= 0) {
                    ESP_LOGI(TAG, "MQTT: %d variaveis enviadas; confira os pontos no Ubidots", dots);
                } else {
                    ESP_LOGW(TAG, "Falha ao publicar MQTT; aguardando proximo intervalo");
                }
            } else if (dots < 0) {
                ESP_LOGE(TAG, "Falha ao montar payload MQTT ou tamanho excedido; envio cancelado");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void ubidots_telemetry_start(void) {
#if CONFIG_BEER_UBIDOTS_ENABLED
    if (!strlen(CONFIG_BEER_WIFI_SSID) || !strlen(CONFIG_BEER_UBIDOTS_TOKEN)) {
        ESP_LOGW(TAG, "Envio desativado: preencha SSID e token no menuconfig");
        return;
    }
    if (!validLabel(CONFIG_BEER_UBIDOTS_DEVICE_LABEL)) {
        ESP_LOGE(TAG, "Device label invalido: use ate 64 letras minusculas/digitos/-/_");
        return;
    }
    mqttEvents = xEventGroupCreate();
    if (!mqttEvents) {
        ESP_LOGE(TAG, "Falha ao criar eventos MQTT");
        return;
    }
    if (xTaskCreate(telemetryTask, "ubidots", 4096, NULL, 1, NULL) != pdPASS) {
        vEventGroupDelete(mqttEvents);
        mqttEvents = NULL;
        ESP_LOGE(TAG, "Falha ao criar tarefa de telemetria");
    }
#endif
}
