#include "IspindelReceiver.h"
#include "TelemetryLogger.h"
#include "BrewProtocol.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"

#if ESP_IDF_VERSION_MAJOR < 5
#error "Este exemplo usa a API de recepcao do ESP-IDF 5 ou superior."
#endif

static const char *TAG = "ISPINDEL";

typedef struct {
    uint8_t mac[6];
    int rssi;
    BrewTelemetry packet;
} RxItem;

static QueueHandle_t rxQueue = NULL;
static QueueHandle_t latestQueue = NULL;

// Executado pela tarefa Wi-Fi: apenas copia e enfileira.
static void onReceive(const esp_now_recv_info_t *info,
                      const uint8_t *data,
                      int length) {
    if (!info || !data ||
        length != (int)(sizeof(BrewTelemetry))) {
        return;
    }

    RxItem item = {0};

    memcpy(item.mac, info->src_addr, sizeof(item.mac));
    memcpy(&item.packet, data, sizeof(item.packet));

    item.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;

    // Não é ISR. Não espera se a fila estiver cheia.
    // Sem ACK, o transmissor poderá repetir a leitura.
    xQueueSend(rxQueue, &item, 0);
}

static bool validReading(const BrewTelemetry *p) {
    if (!brew_header_valid(&p->header, BREW_TELEMETRY)) {
        return false;
    }

    if (!isfinite(p->temperatureC) ||
        !isfinite(p->tiltDegrees) ||
        !isfinite(p->gravity) ||
        !isfinite(p->batteryV)) {
        return false;
    }

    return p->temperatureC >= -55.0f &&
           p->temperatureC <= 125.0f &&
           p->temperatureC != 85.0f &&
           p->tiltDegrees >= 0.0f &&
           p->tiltDegrees <= 90.5f &&
           p->batteryV > 0.0f &&
           p->batteryV <= 6.0f;
}

static esp_err_t acknowledge(const RxItem *item) {
    if (!esp_now_is_peer_exist(item->mac)) {
        esp_now_peer_info_t peer = {0};

        memcpy(peer.peer_addr, item->mac, sizeof(peer.peer_addr));
        peer.channel = BREW_CHANNEL;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;

        esp_err_t result = esp_now_add_peer(&peer);

        if (result != ESP_OK) {
            return result;
        }
    }

    BrewAck ack = item->packet.header;
    ack.type = BREW_ACK;

    return esp_now_send(
        item->mac,
        (const uint8_t *)&ack,
        sizeof(ack)
    );
}

static void receiverTask(void *context) {
    (void)context;
    RxItem item = {0};

    // Seleciona o primeiro sensor válido até reiniciar.
    bool selected = false;
    uint8_t sensorMac[6] = {0};

    bool havePrevious = false;
    BrewHeader previous = {0};

    uint32_t count = 0;
    TickType_t lastValid = xTaskGetTickCount();
    bool absenceReported = false;

    for (;;) {
        if (xQueueReceive(rxQueue, &item,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (!absenceReported &&
                xTaskGetTickCount() - lastValid >
                    pdMS_TO_TICKS(ISPINDEL_STALE_AFTER_MS)) {
                ESP_LOGW(TAG, "Sem telemetria valida ha 35 segundos");
                absenceReported = true;
            }

            continue;
        }

        if (!validReading(&item.packet)) {
            ESP_LOGW(TAG, "Pacote rejeitado na validacao");
            continue;
        }

        if (selected &&
            memcmp(sensorMac, item.mac, sizeof(sensorMac)) != 0) {
            continue;
        }

        if (!selected) {
            memcpy(sensorMac, item.mac, sizeof(sensorMac));
            selected = true;
        }

        const BrewTelemetry *p = &item.packet;

        bool duplicate =
            havePrevious &&
            previous.sensorId == p->header.sensorId &&
            previous.bootId == p->header.bootId &&
            previous.sequence == p->header.sequence;

        // Confirma também retransmissões, caso o ACK anterior se perca.
        esp_err_t result = acknowledge(&item);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Erro ao enfileirar ACK: %s",
                     esp_err_to_name(result));
        }

        lastValid = xTaskGetTickCount();
        absenceReported = false;
        const IspindelReading reading = {*p, lastValid};
        xQueueOverwrite(latestQueue, &reading);

        if (duplicate) {
            ESP_LOGI(TAG, "Retransmissao: ACK reenviado");
            continue;
        }

        previous = p->header;
        havePrevious = true;
        ++count;

        telemetry_logger_print_reading(p, item.mac, item.rssi, count);
    }
}

bool ispindel_receiver_create_queue(void) {
    rxQueue = xQueueCreate(8, sizeof(RxItem));
    latestQueue = xQueueCreate(1, sizeof(IspindelReading));
    return rxQueue != NULL && latestQueue != NULL;
}

bool ispindel_receiver_latest(IspindelReading *reading) {
    return reading && latestQueue && xQueuePeek(latestQueue, reading, 0) == pdTRUE;
}

void ispindel_receiver_start(void) {
    ESP_ERROR_CHECK(esp_now_init());

    if (xTaskCreate(receiverTask, "ispindel_rx",
                    4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa");
        abort();
    }

    ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));

}
