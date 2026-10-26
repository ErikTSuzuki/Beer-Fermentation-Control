#include "TemperatureControl.h"
#include "LocalTemperature.h"
#include "RelayConfig.h"
#include "sdkconfig.h"

#include <inttypes.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "CONTROL";
static const int OFF = RELAY_ACTIVE_LOW ? 1 : 0;
static const int ON = RELAY_ACTIVE_LOW ? 0 : 1;
static const uint32_t PERIOD_MS = 500;
static QueueHandle_t commands;
static QueueHandle_t statuses;
static bool started;
#if CONFIG_BEER_CONTROL_ENABLED
static const bool ENABLED_AT_BOOT = true;
#else
static const bool ENABLED_AT_BOOT = false;
#endif

static const TemperatureControlSettings settings = {
    .minimumC = CONFIG_BEER_CONTROL_MIN_CENTIC / 100.0f,
    .maximumC = CONFIG_BEER_CONTROL_MAX_CENTIC / 100.0f,
    .heatingDifferentialC = CONFIG_BEER_CONTROL_HEATING_DIFF_CENTIC / 100.0f,
    .coolingDifferentialC = CONFIG_BEER_CONTROL_COOLING_DIFF_CENTIC / 100.0f,
    .compressorOffUs = (int64_t)CONFIG_BEER_CONTROL_COMPRESSOR_OFF_SECONDS * 1000000,
    .changeoverUs = (int64_t)CONFIG_BEER_CONTROL_CHANGEOVER_SECONDS * 1000000,
};

static esp_err_t allOff(void) {
    // Tenta ambos, mesmo se o primeiro GPIO retornar erro.
    const esp_err_t heater = gpio_set_level(RELAY_HEATER, OFF);
    const esp_err_t cooler = gpio_set_level(RELAY_COOLER, OFF);
    return heater != ESP_OK ? heater : cooler;
}

static esp_err_t applyOutput(TemperatureControlOutput output) {
    // Unico dono dos GPIOs durante a operacao. Desliga o oposto antes de ligar.
    esp_err_t result;
    if (output == CONTROL_OUTPUT_HEATER) {
        result = gpio_set_level(RELAY_COOLER, OFF);
        if (result == ESP_OK) result = gpio_set_level(RELAY_HEATER, ON);
    } else if (output == CONTROL_OUTPUT_COOLER) {
        result = gpio_set_level(RELAY_HEATER, OFF);
        if (result == ESP_OK) result = gpio_set_level(RELAY_COOLER, ON);
    } else {
        result = allOff();
    }
    if (result != ESP_OK) (void)allOff();
    return result;
}

const char *temperature_control_state_name(TemperatureControlState state) {
    switch (state) {
        case CONTROL_DISABLED: return "DESATIVADO";
        case CONTROL_IDLE: return "EM ESPERA";
        case CONTROL_HEATING: return "AQUECENDO";
        case CONTROL_COOLING: return "RESFRIANDO";
        case CONTROL_WAIT_COMPRESSOR: return "BLOQ COMPRESSOR";
        case CONTROL_WAIT_CHANGEOVER: return "BLOQ TROCA";
        case CONTROL_SENSOR_FAULT: return "FALHA SENSOR";
        case CONTROL_OUTPUT_FAULT: return "FALHA RELE";
        case CONTROL_CONFIG_FAULT: return "FALHA CONFIG";
        default: return "DESCONHECIDO";
    }
}

static void controlTask(void *context) {
    (void)context;
    TemperatureControlCommand command = {
        .targetC = CONFIG_BEER_CONTROL_TARGET_CENTIC / 100.0f,
        .enabled = ENABLED_AT_BOOT,
    };
    TemperatureControlLogic logic;
    temperature_control_logic_initialize(&logic, esp_timer_get_time());
    TemperatureControlOutput appliedOutput = CONTROL_OUTPUT_OFF;
    TemperatureControlState previousState = CONTROL_CONFIG_FAULT;
    bool outputFault = false;
    float previousTarget = NAN;
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        // Nao espera rede, RTC nem conversao do sensor.
        (void)xQueueReceive(commands, &command, 0);
        LocalTemperature sample = {0};
        const bool haveSample = local_temperature_latest(&sample);
        const int64_t nowUs = esp_timer_get_time();
        const bool valid = haveSample && sample.valid && isfinite(sample.temperatureC) &&
            sample.temperatureC >= settings.minimumC && sample.temperatureC <= settings.maximumC &&
            sample.sampledAtUs >= 0 && nowUs >= sample.sampledAtUs &&
            nowUs - sample.sampledAtUs <= (int64_t)CONFIG_BEER_CONTROL_SENSOR_MAX_AGE_MS * 1000;
        TemperatureControlDecision decision = temperature_control_logic_step(
            &logic, &settings, command, sample.temperatureC, valid, nowUs);

        if (outputFault) {
            // Falha de escrita fica travada ate reiniciar; nao tenta novas partidas.
            (void)allOff();
            decision = (TemperatureControlDecision){.state = CONTROL_OUTPUT_FAULT};
        } else if (decision.output != appliedOutput) {
            const esp_err_t result = applyOutput(decision.output);
            if (result != ESP_OK) {
                outputFault = true;
                decision = (TemperatureControlDecision){.state = CONTROL_OUTPUT_FAULT};
                ESP_LOGE(TAG, "Falha ao escrever reles: %s; controle bloqueado ate reiniciar",
                         esp_err_to_name(result));
            } else if (decision.output == CONTROL_OUTPUT_OFF) {
                // Conta a protecao a partir da escrita fisica, nao antes dela.
                const int64_t offAtUs = esp_timer_get_time();
                logic.stoppedAtUs = offAtUs;
                if (appliedOutput == CONTROL_OUTPUT_COOLER) logic.coolerOffAtUs = offAtUs;
            }
        }
        appliedOutput = decision.output;

        const TemperatureControlStatus status = {
            .state = decision.state,
            .targetC = command.targetC,
            .measuredC = valid ? sample.temperatureC : NAN,
            .sensorValid = valid,
            .enabled = command.enabled,
            .heaterOn = decision.output == CONTROL_OUTPUT_HEATER,
            .coolerOn = decision.output == CONTROL_OUTPUT_COOLER,
            .waitSeconds = (uint32_t)((decision.waitUs + 999999) / 1000000),
            .updatedAtUs = nowUs,
        };
        xQueueOverwrite(statuses, &status);
        if (status.state != previousState || status.targetC != previousTarget) {
            ESP_LOGI(TAG, "%s | alvo=%.2f C | sensor=%s | aquecedor=%d refrigerador=%d | espera=%" PRIu32 " s",
                     temperature_control_state_name(status.state), (double)status.targetC,
                     status.sensorValid ? "OK" : "INDISPONIVEL", status.heaterOn,
                     status.coolerOn, status.waitSeconds);
            previousState = status.state;
            previousTarget = status.targetC;
        }
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t temperature_control_initialize(void) {
    if (commands || statuses) return ESP_ERR_INVALID_STATE;
    esp_err_t result = allOff();
    if (result != ESP_OK) return result;
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << RELAY_HEATER) | (1ULL << RELAY_COOLER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = RELAY_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = RELAY_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    result = gpio_config(&config);
    if (result != ESP_OK) return result;
    result = allOff();
    if (result != ESP_OK) return result;
    const float target = CONFIG_BEER_CONTROL_TARGET_CENTIC / 100.0f;
    if (!temperature_control_settings_valid(&settings, target)) return ESP_ERR_INVALID_ARG;

    commands = xQueueCreate(1, sizeof(TemperatureControlCommand));
    statuses = xQueueCreate(1, sizeof(TemperatureControlStatus));
    if (!commands || !statuses) {
        if (commands) vQueueDelete(commands);
        if (statuses) vQueueDelete(statuses);
        commands = NULL;
        statuses = NULL;
        return ESP_ERR_NO_MEM;
    }
    const TemperatureControlStatus initial = {
        .state = ENABLED_AT_BOOT ? CONTROL_SENSOR_FAULT : CONTROL_DISABLED,
        .enabled = ENABLED_AT_BOOT,
        .targetC = target,
        .measuredC = NAN,
        .updatedAtUs = esp_timer_get_time(),
    };
    xQueueOverwrite(statuses, &initial);
    ESP_LOGI(TAG, "Aquecedor=GPIO%d refrigerador=GPIO%d ativo em %s; ambos desligados",
             (int)RELAY_HEATER, (int)RELAY_COOLER, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
    return ESP_OK;
}

esp_err_t temperature_control_start(void) {
    if (!commands || !statuses || started) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(controlTask, "temperature_control", 4096, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    started = true;
    return ESP_OK;
}

bool temperature_control_set_target(float targetC, bool enabled) {
    if (!commands || !temperature_control_target_valid(targetC)) return false;
    const TemperatureControlCommand command = {.targetC = targetC, .enabled = enabled};
    return xQueueOverwrite(commands, &command) == pdTRUE;
}

bool temperature_control_target_valid(float targetC) {
    return temperature_control_settings_valid(&settings, targetC);
}

void temperature_control_get_settings(TemperatureControlSettings *current) {
    if (current) *current = settings;
}

bool temperature_control_latest(TemperatureControlStatus *status) {
    return status && statuses && xQueuePeek(statuses, status, 0) == pdTRUE;
}
