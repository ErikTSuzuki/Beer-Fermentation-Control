#include "BrewJson.h"

#include <float.h>
#include <math.h>
#include <string.h>
#include "cJSON.h"

static bool readNumber(const cJSON *object, const char *name, double *value) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble)) return false;
    *value = field->valuedouble;
    return true;
}

static bool readUint32(const cJSON *object, const char *name, uint32_t *value) {
    double number;
    if (!readNumber(object, name, &number) || number < 0 ||
        number > UINT32_MAX || floor(number) != number) return false;
    *value = (uint32_t)number;
    return true;
}

static bool readFloat(const cJSON *object, const char *name, float *value) {
    double number;
    if (!readNumber(object, name, &number) || number < -FLT_MAX || number > FLT_MAX) return false;
    *value = (float)number;
    return isfinite(*value);
}

bool brew_json_parse(const char *body, size_t length, BrewTelemetry *packet) {
    if (!body || !packet || !length || length >= BREW_HTTP_BODY_MAX ||
        memchr(body, '\0', length)) return false;

    // O contrato e um objeto plano de numeros. Limita a profundidade antes
    // do parser para que corpos pequenos nao esgotem a pilha do servidor.
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        const char c = body[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || (c == '{' && ++depth > 1)) return false;
    }

    char terminated[BREW_HTTP_BODY_MAX];
    memcpy(terminated, body, length);
    terminated[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(terminated, length + 1, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    // Campos repetidos sao ambiguos mesmo quando o primeiro valor e valido.
    for (const cJSON *a = root->child; a; a = a->next) {
        for (const cJSON *b = a->next; b; b = b->next) {
            if (!strcmp(a->string, b->string)) {
                cJSON_Delete(root);
                return false;
            }
        }
    }

    BrewTelemetry result = {0};
    uint32_t version = 0, unit = 0;
    const bool valid =
        readUint32(root, "version", &version) && version == BREW_VERSION &&
        readUint32(root, "sensor_id", &result.header.sensorId) && result.header.sensorId &&
        readUint32(root, "boot_id", &result.header.bootId) && result.header.bootId &&
        readUint32(root, "sequence", &result.header.sequence) && result.header.sequence &&
        readUint32(root, "gravity_unit", &unit) && unit <= BREW_UNIT_PLATO &&
        readFloat(root, "temperature_c", &result.temperatureC) &&
        readFloat(root, "tilt_degrees", &result.tiltDegrees) &&
        readFloat(root, "gravity", &result.gravity) &&
        readFloat(root, "battery_v", &result.batteryV) &&
        result.temperatureC >= -55.0f && result.temperatureC <= 125.0f &&
        result.temperatureC != 85.0f &&
        result.tiltDegrees >= 0.0f && result.tiltDegrees <= 90.5f &&
        result.batteryV > 0.0f && result.batteryV <= 6.0f;
    cJSON_Delete(root);
    if (!valid) return false;

    // A estrutura anterior continua sendo usada internamente pela tela/nuvem.
    result.header.magic = BREW_MAGIC;
    result.header.version = (uint8_t)version;
    result.header.type = BREW_TELEMETRY;
    result.header.gravityUnit = (uint8_t)unit;
    *packet = result;
    return true;
}
