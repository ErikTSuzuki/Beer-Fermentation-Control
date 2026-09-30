#include "LocalTemperature.h"

#include <cmath>
#include <cstdint>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "ds18b20.h"

namespace {

// DS18B20 alimentado por 3,3 V: DQ no GPIO4 e pull-up externo de 4,7 kohm.
static constexpr int DS18B20_GPIO = 4;
static constexpr uint32_t LOCAL_SAMPLE_PERIOD_MS = 2000;
static const char *LOCAL_TAG = "DS18B20_LOCAL";

// Caixa postal com a amostra mais recente; acesso seguro entre as tarefas.
static QueueHandle_t localTemperatureQueue = nullptr;

static esp_err_t findLocalSensor(onewire_bus_handle_t bus,
                                 ds18b20_device_handle_t *sensor) {
    onewire_device_iter_handle_t iter = nullptr;
    esp_err_t result = onewire_new_device_iter(bus, &iter);
    if (result != ESP_OK) {
        return result;
    }

    onewire_device_t device = {};
    while ((result = onewire_device_iter_get_next(iter, &device)) == ESP_OK) {
        ds18b20_config_t config = {};
        result = ds18b20_new_device_from_enumeration(&device, &config, sensor);
        if (result != ESP_ERR_NOT_SUPPORTED) {
            break;
        }
    }
    onewire_del_device_iter(iter);
    return result;
}

static void localTemperatureTask(void *) {
    onewire_bus_handle_t bus = nullptr;
    onewire_bus_config_t busConfig = {};
    busConfig.bus_gpio_num = DS18B20_GPIO;
    onewire_bus_rmt_config_t rmtConfig = {};
    rmtConfig.max_rx_bytes = 10;

    esp_err_t result = onewire_new_bus_rmt(&busConfig, &rmtConfig, &bus);
    if (result != ESP_OK) {
        ESP_LOGE(LOCAL_TAG, "Falha ao iniciar GPIO%d: %s",
                 DS18B20_GPIO, esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    ds18b20_device_handle_t sensor = nullptr;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        LocalTemperature sample = {};
        result = ESP_OK;
        if (!sensor) {
            result = findLocalSensor(bus, &sensor);
            if (result == ESP_OK) {
                result = ds18b20_set_resolution(sensor, DS18B20_RESOLUTION_12B);
            }
        }
        if (result == ESP_OK) {
            // A biblioteca espera a conversao nesta tarefa, sem bloquear o ACK.
            result = ds18b20_trigger_temperature_conversion(sensor);
        }
        if (result == ESP_OK) {
            result = ds18b20_get_temperature(sensor, &sample.temperatureC);
        }
        if (result == ESP_OK &&
            (!std::isfinite(sample.temperatureC) ||
             sample.temperatureC < -55.0f || sample.temperatureC > 125.0f ||
             sample.temperatureC == 85.0f)) {
            // 85 C e o valor de inicializacao; inadequado para esta comparacao.
            result = ESP_ERR_INVALID_RESPONSE;
        }

        sample.valid = result == ESP_OK;
        sample.sampledAt = xTaskGetTickCount();
        xQueueOverwrite(localTemperatureQueue, &sample);
        if (sample.valid) {
            ESP_LOGI(LOCAL_TAG, "GPIO%d | Temperatura=%.2f C",
                     DS18B20_GPIO, static_cast<double>(sample.temperatureC));
        } else {
            ESP_LOGW(LOCAL_TAG, "Sensor indisponivel no GPIO%d: %s; tentando novamente",
                     DS18B20_GPIO, esp_err_to_name(result));
            if (sensor) {
                ds18b20_del_device(sensor);
                sensor = nullptr;
            }
        }
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(LOCAL_SAMPLE_PERIOD_MS));
    }
}

} // namespace

namespace local_temperature {

bool createQueue() {
    localTemperatureQueue = xQueueCreate(1, sizeof(LocalTemperature));
    return localTemperatureQueue != nullptr;
}

bool latest(LocalTemperature &sample) {
    return xQueuePeek(localTemperatureQueue, &sample, 0) == pdTRUE;
}

void start() {
    if (xTaskCreate(localTemperatureTask, "ds18b20_local",
                    4096, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(LOCAL_TAG, "Falha ao criar tarefa; sensor local indisponivel");
    }

}

} // namespace local_temperature
