#include "UbidotsTelemetry.h"
#include "sdkconfig.h"

#if CONFIG_BEER_UBIDOTS_ENABLED
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "WifiStation.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
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
    const bool haveLocal = local_temperature_latest(&local);
    const bool haveRemote = ispindel_receiver_latest(&remote);
    const TickType_t now = xTaskGetTickCount();
    const bool localOk = haveLocal && local.valid && isfinite(local.temperatureC) &&
        now - local.sampledAt <= pdMS_TO_TICKS(5000);
    const BrewTelemetry *p = &remote.packet;
    const bool remoteOk = haveRemote &&
        now - remote.receivedAt <= ISPINDEL_STALE_AFTER_TICKS &&
        isfinite(p->temperatureC) && isfinite(p->tiltDegrees) &&
        isfinite(p->gravity) && isfinite(p->batteryV);
    if (!localOk && !remoteOk) return 0;

    int length = snprintf(payload, capacity, "{");
    int dots = 0;
    if (localOk) {
        length = snprintf(payload, capacity, "{\"temperatura-local\":%.3f",
                          (double)local.temperatureC);
        dots = 1;
    }
    if (length < 0 || (size_t)length >= capacity) return -1;
    if (remoteOk) {
        const char *unit = p->header.gravityUnit == BREW_UNIT_SG ? "sg" :
            p->header.gravityUnit == BREW_UNIT_PLATO ? "plato" : "unknown";
        const int written = snprintf(payload + length, capacity - (size_t)length,
            "%s\"temperatura-ispindel\":%.3f,\"inclinacao\":%.3f,"
            "\"densidade\":{\"value\":%.6f,\"context\":{\"unit\":\"%s\","
            "\"sensor_id\":\"%06" PRIX32 "\"}},\"bateria\":%.3f",
            dots ? "," : "", (double)p->temperatureC, (double)p->tiltDegrees,
            (double)p->gravity, unit, p->header.sensorId, (double)p->batteryV);
        if (written < 0 || (size_t)written >= capacity - (size_t)length) return -1;
        length += written;
        dots += 4;
    }
    if ((size_t)length + 2 > capacity) return -1;
    payload[length++] = '}';
    payload[length] = '\0';
    return dots;
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
        .buffer.size = 1024,
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
            char payload[768];
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
                ESP_LOGE(TAG, "Payload excede o buffer; envio cancelado");
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
