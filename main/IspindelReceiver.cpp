#include "IspindelReceiver.h"
#include "TelemetryLogger.h"
#include "BrewProtocol.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
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

namespace {

struct RxItem {
    uint8_t mac[6];
    int rssi;
    BrewTelemetry packet;
};

static QueueHandle_t rxQueue = nullptr;

// Executado pela tarefa Wi-Fi: apenas copia e enfileira.
static void onReceive(const esp_now_recv_info_t *info,
                      const uint8_t *data,
                      int length) {
    if (!info || !data ||
        length != static_cast<int>(sizeof(BrewTelemetry))) {
        return;
    }

    RxItem item = {};

    std::memcpy(item.mac, info->src_addr, sizeof(item.mac));
    std::memcpy(&item.packet, data, sizeof(item.packet));

    item.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -127;

    // Não é ISR. Não espera se a fila estiver cheia.
    // Sem ACK, o transmissor poderá repetir a leitura.
    xQueueSend(rxQueue, &item, 0);
}

static bool validReading(const BrewTelemetry &p) {
    if (!brewHeaderValid(p.header, BREW_TELEMETRY)) {
        return false;
    }

    if (!std::isfinite(p.temperatureC) ||
        !std::isfinite(p.tiltDegrees) ||
        !std::isfinite(p.gravity) ||
        !std::isfinite(p.batteryV)) {
        return false;
    }

    return p.temperatureC >= -55.0f &&
           p.temperatureC <= 125.0f &&
           p.temperatureC != 85.0f &&
           p.tiltDegrees >= 0.0f &&
           p.tiltDegrees <= 90.5f &&
           p.batteryV > 0.0f &&
           p.batteryV <= 6.0f;
}

static esp_err_t acknowledge(const RxItem &item) {
    if (!esp_now_is_peer_exist(item.mac)) {
        esp_now_peer_info_t peer = {};

        std::memcpy(peer.peer_addr, item.mac, sizeof(peer.peer_addr));
        peer.channel = BREW_CHANNEL;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;

        esp_err_t result = esp_now_add_peer(&peer);

        if (result != ESP_OK) {
            return result;
        }
    }

    BrewAck ack = item.packet.header;
    ack.type = BREW_ACK;

    return esp_now_send(
        item.mac,
        reinterpret_cast<const uint8_t *>(&ack),
        sizeof(ack)
    );
}

static void receiverTask(void *) {
    RxItem item = {};

    // Seleciona o primeiro sensor válido até reiniciar.
    bool selected = false;
    uint8_t sensorMac[6] = {};

    bool havePrevious = false;
    BrewHeader previous = {};

    uint32_t count = 0;
    TickType_t lastValid = xTaskGetTickCount();
    bool absenceReported = false;

    for (;;) {
        if (xQueueReceive(rxQueue, &item,
                          pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (!absenceReported &&
                xTaskGetTickCount() - lastValid >
                    pdMS_TO_TICKS(35000)) {
                ESP_LOGW(TAG, "Sem telemetria valida ha 35 segundos");
                absenceReported = true;
            }

            continue;
        }

        if (!validReading(item.packet)) {
            ESP_LOGW(TAG, "Pacote rejeitado na validacao");
            continue;
        }

        if (selected &&
            std::memcmp(sensorMac, item.mac, sizeof(sensorMac)) != 0) {
            continue;
        }

        if (!selected) {
            std::memcpy(sensorMac, item.mac, sizeof(sensorMac));
            selected = true;
        }

        const BrewTelemetry &p = item.packet;

        bool duplicate =
            havePrevious &&
            previous.sensorId == p.header.sensorId &&
            previous.bootId == p.header.bootId &&
            previous.sequence == p.header.sequence;

        // Confirma também retransmissões, caso o ACK anterior se perca.
        esp_err_t result = acknowledge(item);

        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Erro ao enfileirar ACK: %s",
                     esp_err_to_name(result));
        }

        lastValid = xTaskGetTickCount();
        absenceReported = false;

        if (duplicate) {
            ESP_LOGI(TAG, "Retransmissao: ACK reenviado");
            continue;
        }

        previous = p.header;
        havePrevious = true;
        ++count;

        telemetry_logger::printReading(p, item.mac, item.rssi, count);
    }
}

} // namespace

namespace ispindel_receiver {

bool createQueue() {
    rxQueue = xQueueCreate(8, sizeof(RxItem));
    return rxQueue != nullptr;
}

void start() {
    ESP_ERROR_CHECK(esp_now_init());

    if (xTaskCreate(receiverTask, "ispindel_rx",
                    4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa");
        std::abort();
    }

    ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));

}

} // namespace ispindel_receiver
