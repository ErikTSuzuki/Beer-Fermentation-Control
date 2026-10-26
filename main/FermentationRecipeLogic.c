#include "FermentationRecipeLogic.h"

#include <math.h>
#include <string.h>

static bool nameValid(const char *name) {
    return name[0] && memchr(name, '\0', RECIPE_NAME_SIZE);
}

bool fermentation_recipe_gravity_valid(uint8_t unit, float value) {
    return isfinite(value) && (unit == BREW_UNIT_SG
        ? value >= 0.8f && value <= 1.5f
        : unit == BREW_UNIT_PLATO && value >= -10.0f && value <= 50.0f);
}

bool fermentation_recipe_valid(const FermentationRecipe *recipe) {
    if (!recipe || !nameValid(recipe->name) ||
        (recipe->gravityUnit != BREW_UNIT_SG && recipe->gravityUnit != BREW_UNIT_PLATO) ||
        !recipe->confirmationSamples || recipe->confirmationSamples > 10 ||
        !recipe->stepCount || recipe->stepCount > RECIPE_MAX_STEPS) return false;
    for (unsigned i = 0; i < recipe->stepCount; ++i) {
        const FermentationStep *step = &recipe->steps[i];
        if (!nameValid(step->name) || !isfinite(step->targetC) ||
            step->targetC < -55.0f || step->targetC > 125.0f ||
            step->maximumUs < INT64_C(1000000) || step->maximumUs > 365 * RECIPE_DAY_US ||
            (step->hasGravityLimit && !fermentation_recipe_gravity_valid(recipe->gravityUnit, step->gravityLimit))) return false;
    }
    return true;
}

void fermentation_recipe_begin(FermentationRecipeProgress *progress, int64_t nowUs) {
    *progress = (FermentationRecipeProgress){.state = RECIPE_RUNNING,
        .reason = RECIPE_REASON_STARTED, .lastTickUs = nowUs, .enteredAtUs = nowUs};
}

static bool advance(const FermentationRecipe *recipe, FermentationRecipeProgress *p,
                    FermentationRecipeReason reason, int64_t nowUs) {
    p->reason = reason;
    p->confirmationCount = 0;
    if (p->stepIndex + 1 < recipe->stepCount) {
        ++p->stepIndex;
        p->elapsedUs = 0;
        p->enteredAtUs = nowUs;
    } else {
        p->state = RECIPE_COMPLETED; // Mantem o alvo da ultima etapa.
    }
    return true;
}

bool fermentation_recipe_tick(const FermentationRecipe *recipe,
    FermentationRecipeProgress *p, RecipeGravitySample sample,
    int64_t maximumSampleAgeUs, int64_t nowUs) {
    if (p->state != RECIPE_RUNNING) return false;
    if (nowUs < 0 || !fermentation_recipe_valid(recipe) || p->stepIndex >= recipe->stepCount) {
        p->state = RECIPE_FAULT;
        return true;
    }
    const FermentationStep *step = &recipe->steps[p->stepIndex];
    const int64_t delta = nowUs >= p->lastTickUs ? nowUs - p->lastTickUs : 0;
    p->lastTickUs = nowUs;
    // Saturacao evita overflow; o prazo nunca depende da validade da densidade.
    if (p->elapsedUs < step->maximumUs) {
        const int64_t left = step->maximumUs - p->elapsedUs;
        p->elapsedUs += delta < left ? delta : left;
    }

    const bool fresh = sample.valid && sample.unit == recipe->gravityUnit &&
        fermentation_recipe_gravity_valid(sample.unit, sample.gravity) && sample.receivedAtUs >= p->enteredAtUs &&
        sample.receivedAtUs <= nowUs && nowUs - sample.receivedAtUs <= maximumSampleAgeUs;
    const bool sameIdentity = p->havePreviousGravity &&
        sample.sensorId == p->previousGravity.sensorId && sample.bootId == p->previousGravity.bootId &&
        sample.sequence == p->previousGravity.sequence;
    const bool newSample = fresh && (!p->havePreviousGravity ||
        (!sameIdentity && sample.receivedAtUs > p->previousGravity.receivedAtUs));

    if (!fresh) p->confirmationCount = 0;
    else if (newSample) {
        // Uma lacuna longa ou troca de sensor reinicia a confirmacao.
        if (p->havePreviousGravity && (sample.sensorId != p->previousGravity.sensorId ||
            sample.receivedAtUs - p->previousGravity.receivedAtUs > maximumSampleAgeUs))
            p->confirmationCount = 0;
        p->previousGravity = sample;
        p->havePreviousGravity = true;
        if (step->hasGravityLimit && sample.gravity <= step->gravityLimit) ++p->confirmationCount;
        else p->confirmationCount = 0;
    }

    // Em empate, registra tempo; no maximo uma transicao por ciclo.
    if (p->elapsedUs >= step->maximumUs)
        return advance(recipe, p, RECIPE_REASON_TIME, nowUs);
    if (step->hasGravityLimit && p->confirmationCount >= recipe->confirmationSamples)
        return advance(recipe, p, RECIPE_REASON_GRAVITY, nowUs);
    return false;
}
