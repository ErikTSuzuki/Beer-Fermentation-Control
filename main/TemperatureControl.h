#ifndef TEMPERATURE_CONTROL_H
#define TEMPERATURE_CONTROL_H

#include "esp_err.h"
#include "TemperatureControlLogic.h"

typedef struct {
    TemperatureControlState state;
    float targetC;
    float measuredC;
    bool sensorValid;
    bool enabled;
    bool heaterOn;
    bool coolerOn;
    uint32_t waitSeconds;
    int64_t updatedAtUs;
} TemperatureControlStatus;

// Chamar uma vez em app_main, antes de qualquer tarefa/rede: inicializa GPIOs
// desligados e as caixas postais. Nao inicia o controle ainda.
esp_err_t temperature_control_initialize(void);
// Chamar apos criar a fila e iniciar a tarefa do DS18B20 local.
esp_err_t temperature_control_start(void);
// Interface para o futuro executor de receitas. Copia nao bloqueante: o comando
// mais recente substitui o pendente e e aplicado em ate um periodo (500 ms).
// false = nao inicializado ou alvo fora dos limites. Nao persiste em flash.
bool temperature_control_set_target(float targetC, bool enabled);
// Valida sem enfileirar nem alterar o alvo atual (usado antes de aceitar receitas).
bool temperature_control_target_valid(float targetC);
// Limites imutaveis configurados no firmware, para validacao do editor local.
void temperature_control_get_settings(TemperatureControlSettings *current);
bool temperature_control_latest(TemperatureControlStatus *status);
const char *temperature_control_state_name(TemperatureControlState state);

#endif
