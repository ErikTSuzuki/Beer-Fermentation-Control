#ifndef FERMENTATION_RECIPE_LOGIC_H
#define FERMENTATION_RECIPE_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "BrewProtocol.h"

#define RECIPE_MAX_STEPS 16
#define RECIPE_NAME_SIZE 49
#define RECIPE_JSON_MAX 4096
#define RECIPE_DAY_US INT64_C(86400000000)

typedef struct {
    char name[RECIPE_NAME_SIZE];
    float targetC;
    int64_t maximumUs;
    float gravityLimit;
    bool hasGravityLimit;
} FermentationStep;

typedef struct {
    char name[RECIPE_NAME_SIZE];
    uint8_t gravityUnit;
    uint8_t confirmationSamples;
    uint8_t stepCount;
    FermentationStep steps[RECIPE_MAX_STEPS];
} FermentationRecipe;

typedef enum { RECIPE_IDLE, RECIPE_RUNNING, RECIPE_COMPLETED, RECIPE_FAULT } FermentationRecipeState;
typedef enum { RECIPE_REASON_NONE, RECIPE_REASON_STARTED, RECIPE_REASON_GRAVITY,
               RECIPE_REASON_TIME, RECIPE_REASON_CANCELLED } FermentationRecipeReason;

typedef struct {
    bool valid;
    uint8_t unit;
    float gravity;
    int64_t receivedAtUs;
    uint32_t sensorId, bootId, sequence;
} RecipeGravitySample;

typedef struct {
    FermentationRecipeState state;
    FermentationRecipeReason reason;
    uint8_t stepIndex;
    uint8_t confirmationCount;
    int64_t elapsedUs;
    int64_t lastTickUs;
    int64_t enteredAtUs;
    bool havePreviousGravity;
    RecipeGravitySample previousGravity;
} FermentationRecipeProgress;

bool fermentation_recipe_valid(const FermentationRecipe *recipe);
bool fermentation_recipe_gravity_valid(uint8_t unit, float value);
bool fermentation_recipe_parse(const char *json, size_t length, FermentationRecipe *recipe);
void fermentation_recipe_begin(FermentationRecipeProgress *progress, int64_t nowUs);
// true quando muda de etapa ou conclui. Nunca acessa GPIO, rede, RTC ou flash.
bool fermentation_recipe_tick(const FermentationRecipe *recipe,
    FermentationRecipeProgress *progress, RecipeGravitySample sample,
    int64_t maximumSampleAgeUs, int64_t nowUs);

#endif
