#include "DisplayTest.h"
#include "Mar2406Config.h"
#include "Mar2406Display.h"
#include "Mar2406Touch.h"
#include "FermentationRecipe.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "RtcClock.h"
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
    BORDER = 0x40545D,
    TEXT = 0xF3F5F2,
    MUTED = 0xB9CCD4,
    AMBER = 0xF4BE66,
    GREEN = 0x81D7AC,
    BLUE = 0x83CDF2,
    RED = 0xFF9B91,
    NAV = 0x335668,
    NAV_PRESSED = 0x4B7587,
    BUFFER_ROWS = 16
};

typedef enum {
    PAGE_TEMPERATURE,
    PAGE_RECIPE,
    PAGE_GRAVITY,
    PAGE_ISPINDEL,
    PAGE_SYSTEM,
    PAGE_COUNT
} Page;

typedef struct {
    const char *title;
    const char *metric;
    uint32_t accent;
} PageStyle;

static const PageStyle pages[PAGE_COUNT] = {
    [PAGE_TEMPERATURE] = {"Temperatura", "Temperatura local", BLUE},
    [PAGE_RECIPE] = {"Receita", "Etapa atual", AMBER},
    [PAGE_GRAVITY] = {"Densidade", "Densidade do iSpindel", GREEN},
    [PAGE_ISPINDEL] = {"iSpindel", "Temperatura do iSpindel", BLUE},
    [PAGE_SYSTEM] = {"Sistema", "Hora local", AMBER},
};

typedef struct {
    Page page;
    lv_obj_t *title;
    lv_obj_t *wifi;
    lv_obj_t *metric;
    lv_obj_t *number;
    lv_obj_t *unit;
    lv_obj_t *detail;
    lv_obj_t *status;
    lv_obj_t *progress;
    lv_obj_t *next;
    lv_obj_t *dots[PAGE_COUNT];
} Dashboard;

typedef struct {
    char number[24];
    const char *unit;
    char detail[40];
    const char *status;
    uint32_t statusColor;
    int progress;
} TelemetryView;

_Alignas(LV_DRAW_BUF_ALIGN) static uint8_t drawBuffer[MAR2406_WIDTH * BUFFER_ROWS * 2];

static uint32_t tickMs(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flushDisplay(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const lv_draw_buf_t *buffer = lv_display_get_buf_active(display);
    mar2406_display_draw(area->x1, area->y1, lv_area_get_width(area),
                         lv_area_get_height(area), pixels, buffer->header.stride);
    lv_display_flush_ready(display);
    vTaskDelay(1);
}

static void readTouch(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static lv_point_t lastPoint;
    static bool pending;
    static bool pressed;
    int x, y;
    const bool detected = mar2406_touch_read(&x, &y);

    // Exige duas leituras consecutivas para reconhecer toque e soltura.
    if (detected == pending) {
        pressed = detected;
        if (pressed) lastPoint = (lv_point_t){.x = x, .y = y};
    }
    pending = detected;
    data->point = lastPoint;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int width, int height,
                      int radius, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    return obj;
}

static void show(lv_obj_t *obj, const char *text, uint32_t color) {
    if (strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
    const lv_color_t foreground = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), foreground)) {
        lv_obj_set_style_text_color(obj, foreground, 0);
    }
}

static bool currentStatus(int64_t updatedAtUs, int64_t maximumAgeUs) {
    const int64_t ageUs = esp_timer_get_time() - updatedAtUs;
    return ageUs >= 0 && ageUs <= maximumAgeUs;
}

static void readTemperature(TelemetryView *view) {
    LocalTemperature sample;
    const bool localOk = local_temperature_latest(&sample) && sample.valid &&
        xTaskGetTickCount() - sample.sampledAt <= pdMS_TO_TICKS(5000);
    view->unit = "°C";
    if (localOk) {
        snprintf(view->number, sizeof(view->number), "%.2f", (double)sample.temperatureC);
    }

    TemperatureControlStatus control;
    if (!temperature_control_latest(&control) || !currentStatus(control.updatedAtUs, 2000000)) {
        view->status = "Controle sem sinal";
        view->statusColor = RED;
        return;
    }
    if (control.enabled) {
        snprintf(view->detail, sizeof(view->detail), "Alvo %.1f °C", (double)control.targetC);
    }
    if (control.waitSeconds) {
        const size_t end = strlen(view->detail);
        snprintf(view->detail + end, sizeof(view->detail) - end, "%s%lu s",
                 end > 0 ? "   /   " : "", (unsigned long)control.waitSeconds);
    }
    switch (control.state) {
        case CONTROL_DISABLED: view->status = "Inativo"; break;
        case CONTROL_IDLE: view->status = "Aguardando"; break;
        case CONTROL_HEATING: view->status = "Aquecendo"; view->statusColor = AMBER; break;
        case CONTROL_COOLING: view->status = "Resfriando"; view->statusColor = BLUE; break;
        case CONTROL_WAIT_COMPRESSOR:
        case CONTROL_WAIT_CHANGEOVER:
            view->status = "Em espera";
            view->statusColor = AMBER;
            break;
        case CONTROL_SENSOR_FAULT:
            view->status = "Falha no sensor";
            view->statusColor = RED;
            break;
        default: view->status = "Falha no controle"; view->statusColor = RED; break;
    }
}

static void readRecipe(TelemetryView *view) {
    FermentationRecipeStatus recipe;
    if (!fermentation_recipe_latest(&recipe) || !currentStatus(recipe.updatedAtUs, 2500000)) {
        view->status = "Sem resposta";
        view->statusColor = RED;
    } else if (!recipe.storageOk || recipe.state == RECIPE_FAULT) {
        view->status = recipe.storageOk ? "Falha na receita" : "Erro ao salvar";
        view->statusColor = RED;
    } else if (recipe.state == RECIPE_RUNNING || recipe.state == RECIPE_COMPLETED) {
        snprintf(view->number, sizeof(view->number), "%u / %u",
                 (unsigned)recipe.stepIndex + 1, (unsigned)recipe.stepCount);
        if (recipe.state == RECIPE_COMPLETED) {
            view->status = "Finalizada";
            view->statusColor = GREEN;
        } else {
            snprintf(view->detail, sizeof(view->detail), "%.1f / %.1f dias",
                     recipe.elapsedDays, recipe.maximumDays);
            const double fraction = recipe.maximumDays > 0 ? recipe.elapsedDays / recipe.maximumDays : 0;
            view->progress = fraction >= 1 ? 100 : fraction > 0 ? (int)(fraction * 100) : 0;
            view->status = "Em andamento";
            view->statusColor = AMBER;
        }
    } else {
        view->status = "Sem receita ativa";
    }
}

static void readIspindel(TelemetryView *view, Page page) {
    IspindelReading reading;
    if (!ispindel_receiver_latest(&reading) ||
        !ispindel_reading_is_current(&reading, esp_timer_get_time())) {
        view->status = "Sem sinal";
        view->statusColor = RED;
        return;
    }

    const BrewTelemetry *packet = &reading.packet;
    if (page == PAGE_GRAVITY) {
        const uint8_t unit = packet->header.gravityUnit;
        const char *format = unit == BREW_UNIT_SG ? "%.4f" : "%.2f";
        if (packet->gravity < -100 || packet->gravity >= 1000) format = "%.3g";
        snprintf(view->number, sizeof(view->number), format, (double)packet->gravity);
        view->unit = unit == BREW_UNIT_SG ? "SG" : unit == BREW_UNIT_PLATO ? "°P" : "";
        view->status = "Recebendo";
        view->statusColor = GREEN;
    } else {
        snprintf(view->number, sizeof(view->number), "%.2f", (double)packet->temperatureC);
        snprintf(view->detail, sizeof(view->detail), "Bateria %.2f V   /   Incl. %.1f°",
                 (double)packet->batteryV, (double)packet->tiltDegrees);
        view->unit = "°C";
    }
}

static void readClock(TelemetryView *view) {
    char stamp[24];
    if (rtc_clock_timestamp(stamp, sizeof(stamp))) {
        snprintf(view->number, sizeof(view->number), "%s", stamp + 11);
        snprintf(view->detail, sizeof(view->detail), "%.2s/%.2s/%.4s", stamp + 8, stamp + 5, stamp);
    } else {
        view->status = "RTC sem sinal";
        view->statusColor = RED;
    }
}

static void refreshDashboard(const Dashboard *ui) {
    const bool connected = wifi_station_is_connected();
    show(ui->wifi, connected ? "Wi-Fi online" : "Wi-Fi offline", connected ? GREEN : RED);

    TelemetryView view = {.number = "--", .unit = "", .status = "", .statusColor = MUTED,
                          .progress = -1};
    switch (ui->page) {
        case PAGE_TEMPERATURE: readTemperature(&view); break;
        case PAGE_RECIPE: readRecipe(&view); break;
        case PAGE_GRAVITY:
        case PAGE_ISPINDEL: readIspindel(&view, ui->page); break;
        case PAGE_SYSTEM: readClock(&view); break;
        case PAGE_COUNT: return;
    }

    const PageStyle *style = &pages[ui->page];
    show(ui->title, style->title, TEXT);
    show(ui->metric, style->metric, MUTED);
    show(ui->number, view.number, strcmp(view.number, "--") == 0 ? MUTED : style->accent);
    show(ui->unit, view.unit, style->accent);
    if (*view.unit) lv_obj_align_to(ui->unit, ui->number, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -7);
    show(ui->detail, view.detail, TEXT);
    show(ui->status, view.status, view.statusColor);

    if (view.progress < 0) {
        lv_obj_add_flag(ui->progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_bar_set_value(ui->progress, view.progress, LV_ANIM_OFF);
        lv_obj_remove_flag(ui->progress, LV_OBJ_FLAG_HIDDEN);
    }
    for (int page = 0; page < PAGE_COUNT; ++page) {
        const uint32_t color = page == (int)ui->page ? style->accent : BORDER;
        const lv_color_t dotColor = lv_color_hex(color);
        if (!lv_color_eq(lv_obj_get_style_bg_color(ui->dots[page], 0), dotColor)) {
            lv_obj_set_style_bg_color(ui->dots[page], dotColor, 0);
        }
    }
}

static void navigate(lv_event_t *event) {
    Dashboard *ui = lv_event_get_user_data(event);
    const int direction = lv_event_get_current_target(event) == ui->next ? 1 : -1;
    ui->page = (Page)((ui->page + PAGE_COUNT + direction) % PAGE_COUNT);
    refreshDashboard(ui);
}

static lv_obj_t *navigationButton(lv_obj_t *screen, int x, const char *symbol, Dashboard *ui) {
    lv_obj_t *button = panel(screen, x, 182, 92, 52, 12, NAV);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(button, 6);
    lv_obj_set_style_bg_color(button, lv_color_hex(NAV_PRESSED), LV_STATE_PRESSED);
    lv_obj_t *arrow = label(button, 0, 0, &lv_font_montserrat_36);
    show(arrow, symbol, TEXT);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(button, navigate, LV_EVENT_CLICKED, ui);
    return button;
}

static void createDashboard(lv_obj_t *screen, Dashboard *ui) {
    const lv_font_t *regular = &lv_font_montserrat_14;
    const lv_font_t *medium = &lv_font_montserrat_20;
#if LV_FONT_MONTSERRAT_48
    const lv_font_t *large = &lv_font_montserrat_48;
#else
    const lv_font_t *large = &lv_font_montserrat_36;
#endif

    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    ui->title = label(screen, 12, 12, medium);
    lv_obj_t *wifiPanel = panel(screen, 202, 10, 106, 28, 8, PANEL);
    ui->wifi = label(wifiPanel, 0, 0, regular);
    lv_obj_center(ui->wifi);

    lv_obj_t *card = panel(screen, 12, 46, 296, 129, 12, PANEL);
    ui->metric = label(card, 14, 10, regular);
    ui->number = label(card, 14, 27, large);
    ui->unit = label(card, 0, 0, medium);
    ui->detail = label(card, 14, 103, regular);
    ui->status = label(card, 0, 0, regular);
    lv_obj_align(ui->status, LV_ALIGN_TOP_RIGHT, -14, 103);

    ui->progress = lv_bar_create(card);
    lv_obj_remove_style_all(ui->progress);
    lv_obj_set_pos(ui->progress, 14, 89);
    lv_obj_set_size(ui->progress, 268, 6);
    lv_obj_set_style_radius(ui->progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->progress, lv_color_hex(BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ui->progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui->progress, lv_color_hex(AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui->progress, LV_OPA_COVER, LV_PART_INDICATOR);

    navigationButton(screen, 8, LV_SYMBOL_LEFT, ui);
    ui->next = navigationButton(screen, 220, LV_SYMBOL_RIGHT, ui);
    for (int page = 0; page < PAGE_COUNT; ++page) {
        ui->dots[page] = panel(screen, 156 + (page - PAGE_COUNT / 2) * 17, 204, 8, 8, 4, BORDER);
    }
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
        ESP_LOGE(TAG, "Sem memoria para a interface LVGL");
        lv_deinit();
        vTaskDelete(NULL);
        return;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, drawBuffer, NULL, sizeof(drawBuffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flushDisplay);

    const esp_err_t touchResult = mar2406_touch_initialize();
    if (touchResult == ESP_OK) {
        lv_indev_t *touch = lv_indev_create();
        if (touch) {
            lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(touch, display);
            lv_indev_set_read_cb(touch, readTouch);
            lv_timer_set_period(lv_indev_get_read_timer(touch), 20);
        } else {
            ESP_LOGE(TAG, "Sem memoria para a entrada touch");
        }
    } else {
        ESP_LOGW(TAG, "Touch indisponivel: %s", esp_err_to_name(touchResult));
    }

    Dashboard ui = {.page = PAGE_TEMPERATURE};
    createDashboard(lv_display_get_screen_active(display), &ui);
    ESP_LOGI(TAG, "Painel LVGL iniciado");

    int64_t nextUpdateUs = 0;
    for (;;) {
        const int64_t nowUs = esp_timer_get_time();
        if (nowUs >= nextUpdateUs) {
            refreshDashboard(&ui);
            nextUpdateUs = nowUs + INT64_C(500000);
        }
        lv_timer_handler();
        vTaskDelay(1);
    }
}

void display_test_start(void) {
    if (!MAR2406_TEST_ENABLED) return;
    if (xTaskCreate(displayTask, "mar2406_monitor", 6144, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar tarefa da tela");
    }
}
