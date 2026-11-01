#include "FermentationRecipeLogic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static bool fieldsValid(const cJSON *object, const char *const *allowed, size_t count) {
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *a = object->child; a; a = a->next) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) if (!strcmp(a->string, allowed[i])) found = true;
        if (!found) return false;
        for (const cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    }
    return true;
}

static bool readName(const cJSON *object, char name[RECIPE_NAME_SIZE]) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, "name");
    if (!cJSON_IsString(field) || !field->valuestring[0] ||
        strlen(field->valuestring) >= RECIPE_NAME_SIZE) return false;
    strcpy(name, field->valuestring);
    return true;
}

static bool structureSafe(const char *json, size_t length) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < length; ++i) {
        const char ch = json[i];
        if (quoted) {
            if (escaped) {
                if (ch == 'u' && i + 4 < length && !memcmp(json + i + 1, "0000", 4)) return false;
                escaped = false;
            } else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 3) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    return !depth && !quoted;
}

bool fermentation_recipe_parse(const char *json, size_t length, FermentationRecipe *recipe) {
    if (!json || !recipe || !length || length > RECIPE_JSON_MAX ||
        memchr(json, '\0', length) || !structureSafe(json, length)) return false;
    char *terminated = malloc(length + 1);
    if (!terminated) return false;
    memcpy(terminated, json, length);
    terminated[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(terminated, length + 1, NULL, true);
    free(terminated);
    static const char *const rootFields[] = {
        "version", "name", "gravity_unit", "gravity_confirm_samples", "on_complete", "steps"};
    static const char *const stepFields[] = {"name", "target_c", "max_days", "gravity_limit"};
    FermentationRecipe result = {.confirmationSamples = 3};
    bool valid = fieldsValid(root, rootFields, sizeof(rootFields) / sizeof(rootFields[0]));
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *unit = cJSON_GetObjectItemCaseSensitive(root, "gravity_unit");
    const cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "gravity_confirm_samples");
    const cJSON *complete = cJSON_GetObjectItemCaseSensitive(root, "on_complete");
    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    valid = valid && cJSON_IsNumber(version) && version->valuedouble == 1 &&
        readName(root, result.name) && cJSON_IsString(unit) && cJSON_IsArray(steps) &&
        (!complete || (cJSON_IsString(complete) && !strcmp(complete->valuestring, "hold")));
    if (valid) {
        if (!strcmp(unit->valuestring, "sg")) result.gravityUnit = BREW_UNIT_SG;
        else if (!strcmp(unit->valuestring, "plato")) result.gravityUnit = BREW_UNIT_PLATO;
        else valid = false;
    }
    if (confirm) {
        if (!cJSON_IsNumber(confirm) || confirm->valuedouble < 1 || confirm->valuedouble > 10 ||
            floor(confirm->valuedouble) != confirm->valuedouble) valid = false;
        else result.confirmationSamples = (uint8_t)confirm->valuedouble;
    }
    const int count = cJSON_GetArraySize(steps);
    valid = valid && count > 0 && count <= RECIPE_MAX_STEPS;
    if (valid) result.stepCount = (uint8_t)count;
    for (int i = 0; valid && i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(steps, i);
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(item, "target_c");
        const cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "max_days");
        const cJSON *gravity = cJSON_GetObjectItemCaseSensitive(item, "gravity_limit");
        FermentationStep *step = &result.steps[i];
        valid = fieldsValid(item, stepFields, sizeof(stepFields) / sizeof(stepFields[0])) &&
            readName(item, step->name) && cJSON_IsNumber(target) && isfinite(target->valuedouble) &&
            target->valuedouble >= -55 && target->valuedouble <= 125 &&
            cJSON_IsNumber(days) && isfinite(days->valuedouble) && days->valuedouble > 0 &&
            days->valuedouble <= 365 && days->valuedouble * (double)RECIPE_DAY_US >= 1000000;
        if (!valid) break;
        step->targetC = (float)target->valuedouble;
        step->maximumUs = (int64_t)llround(days->valuedouble * (double)RECIPE_DAY_US);
        step->hasGravityLimit = gravity && !cJSON_IsNull(gravity);
        if (step->hasGravityLimit) {
            valid = cJSON_IsNumber(gravity) && isfinite(gravity->valuedouble) &&
                gravity->valuedouble >= -10 && gravity->valuedouble <= 50;
            if (valid) step->gravityLimit = (float)gravity->valuedouble;
        }
    }
    cJSON_Delete(root);
    if (!valid || !fermentation_recipe_valid(&result)) return false;
    *recipe = result;
    return true;
}
