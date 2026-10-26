#ifndef TEMPERATURE_CONTROL_LOGIC_H
#define TEMPERATURE_CONTROL_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

// Valores estaveis, tambem publicados na telemetria.
typedef enum {
    CONTROL_DISABLED = 0,
    CONTROL_IDLE = 1,
    CONTROL_HEATING = 2,
    CONTROL_COOLING = 3,
    CONTROL_WAIT_COMPRESSOR = 4,
    CONTROL_WAIT_CHANGEOVER = 5,
    CONTROL_SENSOR_FAULT = 6,
    CONTROL_OUTPUT_FAULT = 7,
    CONTROL_CONFIG_FAULT = 8
} TemperatureControlState;

typedef enum {
    CONTROL_OUTPUT_OFF = 0,
    CONTROL_OUTPUT_HEATER,
    CONTROL_OUTPUT_COOLER
} TemperatureControlOutput;

typedef struct {
    float minimumC;
    float maximumC;
    float heatingDifferentialC;
    float coolingDifferentialC;
    int64_t compressorOffUs;
    int64_t changeoverUs;
} TemperatureControlSettings;

typedef struct {
    float targetC;
    bool enabled;
} TemperatureControlCommand;

typedef struct {
    TemperatureControlOutput output;
    TemperatureControlOutput lastStoppedOutput;
    int64_t stoppedAtUs;
    int64_t coolerOffAtUs;
} TemperatureControlLogic;

typedef struct {
    TemperatureControlState state;
    TemperatureControlOutput output;
    int64_t waitUs;
} TemperatureControlDecision;

bool temperature_control_settings_valid(const TemperatureControlSettings *settings,
                                        float targetC);
void temperature_control_logic_initialize(TemperatureControlLogic *logic, int64_t nowUs);
// Nucleo sem GPIO/RTOS: apenas o chamador aplica a decisao aos reles.
TemperatureControlDecision temperature_control_logic_step(
    TemperatureControlLogic *logic, const TemperatureControlSettings *settings,
    TemperatureControlCommand command, float measuredC, bool sensorValid, int64_t nowUs);

#endif
