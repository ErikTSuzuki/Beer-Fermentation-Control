#include "TemperatureControlLogic.h"

#include <math.h>
#include <stddef.h>

bool temperature_control_settings_valid(const TemperatureControlSettings *s, float targetC) {
    return s && isfinite(targetC) && isfinite(s->minimumC) && isfinite(s->maximumC) &&
        isfinite(s->heatingDifferentialC) && isfinite(s->coolingDifferentialC) &&
        s->minimumC >= -55.0f && s->maximumC <= 125.0f && s->minimumC < s->maximumC &&
        s->heatingDifferentialC > 0.0f && s->coolingDifferentialC > 0.0f &&
        targetC - s->heatingDifferentialC >= s->minimumC &&
        targetC + s->coolingDifferentialC <= s->maximumC &&
        s->compressorOffUs >= INT64_C(60000000) &&
        s->compressorOffUs <= INT64_C(3600000000) &&
        s->changeoverUs >= INT64_C(1000000) &&
        s->changeoverUs <= INT64_C(300000000);
}

void temperature_control_logic_initialize(TemperatureControlLogic *logic, int64_t nowUs) {
    *logic = (TemperatureControlLogic){
        .output = CONTROL_OUTPUT_OFF,
        .lastStoppedOutput = CONTROL_OUTPUT_OFF,
        .stoppedAtUs = nowUs,
        // A primeira partida tambem aguarda o bloqueio completo.
        .coolerOffAtUs = nowUs,
    };
}

static void stopOutput(TemperatureControlLogic *logic, int64_t nowUs) {
    if (logic->output == CONTROL_OUTPUT_OFF) return;
    logic->lastStoppedOutput = logic->output;
    logic->stoppedAtUs = nowUs;
    if (logic->output == CONTROL_OUTPUT_COOLER) logic->coolerOffAtUs = nowUs;
    logic->output = CONTROL_OUTPUT_OFF;
}

static int64_t remaining(int64_t nowUs, int64_t sinceUs, int64_t durationUs) {
    const int64_t elapsed = nowUs >= sinceUs ? nowUs - sinceUs : 0;
    return elapsed < durationUs ? durationUs - elapsed : 0;
}

TemperatureControlDecision temperature_control_logic_step(
    TemperatureControlLogic *logic, const TemperatureControlSettings *settings,
    TemperatureControlCommand command, float measuredC, bool sensorValid, int64_t nowUs) {
    TemperatureControlDecision decision = {.state = CONTROL_IDLE, .output = CONTROL_OUTPUT_OFF};
    if (!temperature_control_settings_valid(settings, command.targetC)) {
        stopOutput(logic, nowUs);
        decision.state = CONTROL_CONFIG_FAULT;
        return decision;
    }
    if (!command.enabled) {
        stopOutput(logic, nowUs);
        decision.state = CONTROL_DISABLED;
        return decision;
    }
    if (!sensorValid || !isfinite(measuredC) ||
        measuredC < settings->minimumC || measuredC > settings->maximumC) {
        stopOutput(logic, nowUs);
        decision.state = CONTROL_SENSOR_FAULT;
        return decision;
    }

    if (logic->output != CONTROL_OUTPUT_OFF) {
        const bool keepRunning = logic->output == CONTROL_OUTPUT_HEATER
            ? measuredC < command.targetC : measuredC > command.targetC;
        if (keepRunning) {
            decision.output = logic->output;
            decision.state = logic->output == CONTROL_OUTPUT_HEATER ? CONTROL_HEATING : CONTROL_COOLING;
        } else {
            // Nunca inverte diretamente: esta iteracao apenas desliga.
            stopOutput(logic, nowUs);
        }
        return decision;
    }

    TemperatureControlOutput requested = CONTROL_OUTPUT_OFF;
    if (measuredC >= command.targetC + settings->coolingDifferentialC)
        requested = CONTROL_OUTPUT_COOLER;
    else if (measuredC <= command.targetC - settings->heatingDifferentialC)
        requested = CONTROL_OUTPUT_HEATER;
    if (requested == CONTROL_OUTPUT_OFF) return decision;

    const int64_t compressorWait = requested == CONTROL_OUTPUT_COOLER
        ? remaining(nowUs, logic->coolerOffAtUs, settings->compressorOffUs) : 0;
    const int64_t changeoverWait = logic->lastStoppedOutput != CONTROL_OUTPUT_OFF &&
        logic->lastStoppedOutput != requested
        ? remaining(nowUs, logic->stoppedAtUs, settings->changeoverUs) : 0;
    if (compressorWait || changeoverWait) {
        decision.waitUs = compressorWait >= changeoverWait ? compressorWait : changeoverWait;
        decision.state = compressorWait >= changeoverWait
            ? CONTROL_WAIT_COMPRESSOR : CONTROL_WAIT_CHANGEOVER;
        return decision;
    }

    logic->output = requested;
    decision.output = requested;
    decision.state = requested == CONTROL_OUTPUT_HEATER ? CONTROL_HEATING : CONTROL_COOLING;
    return decision;
}
