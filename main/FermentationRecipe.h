#ifndef FERMENTATION_RECIPE_H
#define FERMENTATION_RECIPE_H

#include "esp_err.h"
#include "FermentationRecipeLogic.h"

typedef struct {
    FermentationRecipeState state;
    FermentationRecipeReason reason;
    char recipeName[RECIPE_NAME_SIZE];
    char stepName[RECIPE_NAME_SIZE];
    uint8_t stepIndex; // Base zero; a tela deve mostrar +1.
    uint8_t stepCount;
    float targetC;
    double elapsedDays;
    double maximumDays;
    bool hasGravityLimit;
    float gravityLimit;
    uint8_t gravityUnit;
    uint8_t confirmationCount;
    bool gravityAvailable;
    bool storageOk;
    esp_err_t lastError;
    int64_t updatedAtUs;
} FermentationRecipeStatus;

// Apos NVS, filas dos sensores e temperature_control_initialize; ANTES de
// temperature_control_start, para restaurar o alvo antes da primeira atuacao.
esp_err_t fermentation_recipe_start(void);
// Comandos nao bloqueantes. ESP_OK = enfileirado, consulte o estado para
// confirmar a execucao. Receita em andamento nao e substituida silenciosamente.
esp_err_t fermentation_recipe_submit_json(const char *json, size_t length);
// Cancela a receita e solicita desligamento dos dois reles.
esp_err_t fermentation_recipe_cancel(void);
bool fermentation_recipe_latest(FermentationRecipeStatus *status);
// Copia segura da ultima receita iniciada, inclusive apos concluir/cancelar.
bool fermentation_recipe_current(FermentationRecipe *current);

#endif
