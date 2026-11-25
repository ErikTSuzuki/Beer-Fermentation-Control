#include "DisplayTest.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"
#include "FermentationRecipe.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "TemperatureControl.h"
#include "WifiStation.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "DISPLAY";

enum {
    BACKGROUND = 0x10191E,
    PANEL = 0x1B292F,
    BORDER = 0x304147,
    TEXT = 0xF3F5F2,
    MUTED = 0xA4B6BD,
    AMBER = 0xF4BE66,
    GREEN = 0x81D7AC,
    BLUE = 0x83CDF2,
    RED = 0xFF9B91,
    BUFFER_ROWS = 24,
};

typedef struct {
    lv_obj_t *wifi;
    lv_obj_t *temperature;
    lv_obj_t *target;
    lv_obj_t *control;
    lv_obj_t *recipe;
    lv_obj_t *stage;
    lv_obj_t *progress;
    lv_obj_t *elapsed;
    lv_obj_t *density;
    lv_obj_t *battery;
} Dashboard;

_Alignas(LV_DRAW_BUF_ALIGN) static uint8_t drawBuffer[MAR2406_WIDTH * BUFFER_ROWS * 2];

static uint32_t tickMs(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flushDisplay(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const lv_draw_buf_t *buffer = lv_display_get_buf_active(display);
    mar2406_display_draw(area->x1, area->y1, lv_area_get_width(area),
                         lv_area_get_height(area), pixels, buffer->header.stride);
    lv_display_flush_ready(display);
    // Libera CPU entre blocos do LCD para a tarefa idle.
    vTaskDelay(1);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                       uint32_t color, const char *text) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_text(obj, text);
    return obj;
}

static Dashboard createDashboard(lv_obj_t *screen) {
    const lv_font_t *small = &lv_font_montserrat_12;
    const lv_font_t *regular = &lv_font_montserrat_14;
    const lv_font_t *medium = &lv_font_montserrat_20;
    Dashboard ui = {0};

    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    label(screen, 12, 12, medium, TEXT, "Beer Monitor");
    lv_obj_t *wifiPanel = panel(screen, 199, 10, 109, 24);
    ui.wifi = label(wifiPanel, 0, 0, small, MUTED, "");
    lv_obj_center(ui.wifi);

    lv_obj_t *temperaturePanel = panel(screen, 12, 42, 296, 81);
    label(temperaturePanel, 12, 10, small, MUTED, "TEMPERATURA");
    ui.temperature = label(temperaturePanel, 12, 28, &lv_font_montserrat_36, TEXT, "");
    lv_obj_t *divider = panel(temperaturePanel, 166, 13, 1, 55);
    lv_obj_set_style_bg_color(divider, lv_color_hex(BORDER), 0);
    label(temperaturePanel, 179, 10, small, MUTED, "ALVO");
    ui.target = label(temperaturePanel, 179, 28, medium, AMBER, "");
    ui.control = label(temperaturePanel, 179, 58, small, MUTED, "");

    lv_obj_t *recipePanel = panel(screen, 12, 131, 296, 62);
    ui.recipe = label(recipePanel, 12, 8, regular, TEXT, "");
    ui.stage = label(recipePanel, 0, 0, small, AMBER, "");
    lv_obj_align(ui.stage, LV_ALIGN_TOP_RIGHT, -12, 9);
    ui.progress = lv_bar_create(recipePanel);
    lv_obj_remove_style_all(ui.progress);
    lv_obj_set_pos(ui.progress, 12, 30);
    lv_obj_set_size(ui.progress, 272, 5);
    lv_obj_set_style_radius(ui.progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui.progress, lv_color_hex(BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui.progress, lv_color_hex(AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui.progress, LV_OPA_COVER, LV_PART_INDICATOR);
    ui.elapsed = label(recipePanel, 12, 41, small, MUTED, "");

    lv_obj_t *sensorPanel = panel(screen, 12, 201, 296, 29);
    label(sensorPanel, 12, 7, small, MUTED, "iSpindel");
    ui.density = label(sensorPanel, 91, 6, regular, TEXT, "");
    ui.battery = label(sensorPanel, 0, 0, small, MUTED, "");
    lv_obj_align(ui.battery, LV_ALIGN_TOP_RIGHT, -12, 7);
    return ui;
}

static void show(lv_obj_t *obj, const char *text, uint32_t color) {
    if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
    const lv_color_t foreground = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), foreground)) {
        lv_obj_set_style_text_color(obj, foreground, 0);
    }
}

static void updateTemperature(const Dashboard *ui) {
    char value[32];
    LocalTemperature sample = {0};
    const bool localOk = local_temperature_latest(&sample) && sample.valid &&
        xTaskGetTickCount() - sample.sampledAt <= pdMS_TO_TICKS(5000);
    if (localOk) {
        snprintf(value, sizeof(value), "%.1f °C", (double)sample.temperatureC);
        show(ui->temperature, value, TEXT);
    } else {
        show(ui->temperature, "--.- °C", RED);
    }

    TemperatureControlStatus control = {0};
    const bool haveControl = temperature_control_latest(&control);
    const int64_t ageUs = esp_timer_get_time() - control.updatedAtUs;
    if (!haveControl || ageUs < 0 || ageUs > INT64_C(2000000)) {
        show(ui->target, "--.- °C", MUTED);
        show(ui->control, "Sem controle", RED);
        return;
    }

    if (control.enabled) {
        snprintf(value, sizeof(value), "%.1f °C", (double)control.targetC);
        show(ui->target, value, AMBER);
    } else {
        show(ui->target, "--.- °C", MUTED);
    }

    switch (control.state) {
        case CONTROL_DISABLED: show(ui->control, "Inativo", MUTED); break;
        case CONTROL_IDLE: show(ui->control, "Aguardando", MUTED); break;
        case CONTROL_HEATING: show(ui->control, "Aquecendo", AMBER); break;
        case CONTROL_COOLING: show(ui->control, "Resfriando", BLUE); break;
        case CONTROL_WAIT_COMPRESSOR:
        case CONTROL_WAIT_CHANGEOVER:
            snprintf(value, sizeof(value), "Espera %lus", (unsigned long)control.waitSeconds);
            show(ui->control, value, AMBER);
            break;
        case CONTROL_SENSOR_FAULT: show(ui->control, "Falha no sensor", RED); break;
        default: show(ui->control, "Falha interna", RED); break;
    }
}

static void updateRecipe(const Dashboard *ui) {
    FermentationRecipeStatus recipe = {0};
    const bool haveRecipe = fermentation_recipe_latest(&recipe);
    const int64_t ageUs = esp_timer_get_time() - recipe.updatedAtUs;
    const bool current = haveRecipe && ageUs >= 0 && ageUs <= INT64_C(2500000);

    if (current && recipe.storageOk && recipe.state == RECIPE_RUNNING) {
        char value[40];
        show(ui->recipe, "Receita em curso", TEXT);
        snprintf(value, sizeof(value), "Etapa %u/%u", (unsigned)recipe.stepIndex + 1,
                 (unsigned)recipe.stepCount);
        show(ui->stage, value, AMBER);
        snprintf(value, sizeof(value), "%.1f de %.1f dias nesta etapa",
                 recipe.elapsedDays, recipe.maximumDays);
        show(ui->elapsed, value, MUTED);
        const double fraction = recipe.maximumDays > 0 ? recipe.elapsedDays / recipe.maximumDays : 0;
        const int progress = fraction >= 1 ? 100 : fraction > 0 ? (int)(fraction * 100) : 0;
        lv_bar_set_value(ui->progress, progress, LV_ANIM_OFF);
        lv_obj_remove_flag(ui->progress, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    show(ui->stage, "", MUTED);
    lv_obj_add_flag(ui->progress, LV_OBJ_FLAG_HIDDEN);
    if (!current) {
        show(ui->recipe, "Receita sem resposta", RED);
        show(ui->elapsed, "", MUTED);
    } else if (!recipe.storageOk || recipe.state == RECIPE_FAULT) {
        show(ui->recipe, "Falha na receita", RED);
        show(ui->elapsed, recipe.storageOk ? "" : "Erro ao salvar", RED);
    } else if (recipe.state == RECIPE_COMPLETED) {
        show(ui->recipe, "Receita completa", GREEN);
        show(ui->elapsed, "Todas as etapas finalizadas", MUTED);
    } else {
        show(ui->recipe, "Sem receita ativa", MUTED);
        show(ui->elapsed, "", MUTED);
    }
}

static void updateIspindel(const Dashboard *ui) {
    IspindelReading reading = {0};
    const bool haveReading = ispindel_receiver_latest(&reading);
    if (!haveReading || !ispindel_reading_is_current(&reading, esp_timer_get_time())) {
        show(ui->density, "Sem sinal", AMBER);
        show(ui->battery, "", MUTED);
        return;
    }

    char value[32];
    const BrewTelemetry *packet = &reading.packet;
    switch (packet->header.gravityUnit) {
        case BREW_UNIT_SG:
            snprintf(value, sizeof(value), "%.4f SG", (double)packet->gravity);
            break;
        case BREW_UNIT_PLATO:
            snprintf(value, sizeof(value), "%.2f °P", (double)packet->gravity);
            break;
        default:
            snprintf(value, sizeof(value), "%.3f", (double)packet->gravity);
            break;
    }
    show(ui->density, value, TEXT);
    snprintf(value, sizeof(value), "%.2f V", (double)packet->batteryV);
    show(ui->battery, value, MUTED);
}

static void displayTask(void *context) {
    (void)context;
    const esp_err_t result = mar2406_display_initialize();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar tela: %s", esp_err_to_name(result));
        vTaskDelete(NULL);
        return;
    }

    lv_init();
    lv_tick_set_cb(tickMs);
    lv_display_t *display = lv_display_create(MAR2406_WIDTH, MAR2406_HEIGHT);
    if (!display) {
        ESP_LOGE(TAG, "Sem memória para a interface LVGL");
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, drawBuffer, NULL, sizeof(drawBuffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flushDisplay);
    const Dashboard ui = createDashboard(lv_display_get_screen_active(display));
    ESP_LOGI(TAG, "Painel LVGL iniciado");

    int64_t nextUpdateUs = 0;
    for (;;) {
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs >= nextUpdateUs) {
            const bool connected = wifi_station_is_connected();
            show(ui.wifi, connected ? LV_SYMBOL_WIFI " Wi-Fi online" : LV_SYMBOL_WIFI " Wi-Fi offline",
                 connected ? GREEN : RED);
            updateTemperature(&ui);
            updateRecipe(&ui);
            updateIspindel(&ui);
            nextUpdateUs = nowUs + INT64_C(500000);
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void display_test_start(void) {
    if (!MAR2406_TEST_ENABLED) return;
    if (xTaskCreate(displayTask, "mar2406_monitor", 6144, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa da tela");
    }
}
