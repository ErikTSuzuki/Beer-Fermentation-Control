#include "LocalWebUi.h"
#include "FermentationRecipe.h"
#include "IspindelReceiver.h"
#include "LocalTemperature.h"
#include "TemperatureControl.h"
#include "WifiStation.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_timer.h"

static bool (*isLocalClient)(httpd_req_t *);

static void responseHeaders(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
}

static esp_err_t message(httpd_req_t *request, const char *status, const char *body) {
    responseHeaders(request);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    // Erros fecham a sessao, inclusive se houver corpo nao consumido.
    return ESP_FAIL;
}

static bool hostValid(httpd_req_t *request) {
    char host[64];
    return httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) == ESP_OK &&
        (!strcmp(host, CONFIG_BEER_AP_IP) || !strcmp(host, CONFIG_BEER_AP_IP ":80"));
}

#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
static esp_err_t redirectToPortal(httpd_req_t *request) {
    // Apenas navegacao GET/HEAD e cliente local; nunca redireciona comandos.
    if (!isLocalClient || !isLocalClient(request) ||
        (request->method != HTTP_GET && request->method != HTTP_HEAD))
        return message(request, "403 Forbidden", "{\"error\":\"local_access_required\"}");
    responseHeaders(request);
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Location", "http://" CONFIG_BEER_AP_IP "/");
    // Corpo ajuda detectores de portal que nao aceitam resposta vazia.
    const char *body = request->method == HTTP_HEAD ? "" :
        "<!doctype html><html lang=\"pt-BR\"><meta charset=\"utf-8\">"
        "<title>Receitas</title><p>Abra o controle de receitas: "
        "<a href=\"http://" CONFIG_BEER_AP_IP "/\">http://" CONFIG_BEER_AP_IP "/</a></p></html>";
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captiveNotFound(httpd_req_t *request, httpd_err_code_t error) {
    (void)error;
    if (!isLocalClient || !isLocalClient(request))
        return message(request, "403 Forbidden", "{\"error\":\"local_access_required\"}");
    // Rotas da API invalidas continuam sendo erros, nao pagina de login.
    if (!strncmp(request->uri, "/api/", 5) || !strcmp(request->uri, "/ispindel"))
        return message(request, "404 Not Found", "{\"error\":\"not_found\"}");
    return redirectToPortal(request);
}
#endif

static bool authorized(httpd_req_t *request, bool authentication) {
    if (!isLocalClient || !isLocalClient(request) || !hostValid(request)) {
        message(request, "403 Forbidden", "{\"error\":\"local_access_required\"}");
        return false;
    }
    if (!authentication) return true;
    char provided[129] = {0};
    const char *expected = CONFIG_BEER_LOCAL_UI_PASSWORD;
    if (!expected[0]) expected = CONFIG_BEER_AP_PASSWORD;
    bool valid = strlen(expected) >= 8 && strlen(expected) <= 63 &&
        httpd_req_get_hdr_value_str(request, "X-Local-Key", provided, sizeof(provided)) == ESP_OK;
    if (valid) {
        unsigned difference = 0;
        // UTF-8 em hexadecimal permite senhas com acentos em headers HTTP.
        char padded[129] = {0};
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < strlen(expected); ++i) {
            const unsigned char ch = (unsigned char)expected[i];
            padded[i * 2] = hex[ch >> 4];
            padded[i * 2 + 1] = hex[ch & 15];
        }
        for (size_t i = 0; i < sizeof(provided); ++i)
            difference |= (unsigned char)provided[i] ^ (unsigned char)padded[i];
        valid = difference == 0;
    }
    if (!valid) message(request, "401 Unauthorized", "{\"error\":\"invalid_password\"}");
    return valid;
}

static bool mutationAllowed(httpd_req_t *request) {
    if (!authorized(request, true)) return false;
    char origin[80];
    // Exige mesma origem. Nenhuma rota concede CORS/preflight a outros sites.
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        (strcmp(origin, "http://" CONFIG_BEER_AP_IP) &&
         strcmp(origin, "http://" CONFIG_BEER_AP_IP ":80"))) {
        message(request, "403 Forbidden", "{\"error\":\"same_origin_required\"}");
        return false;
    }
    return true;
}

static esp_err_t sendObject(httpd_req_t *request, cJSON *object) {
    char *json = object ? cJSON_PrintUnformatted(object) : NULL;
    cJSON_Delete(object);
    if (!json) return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    responseHeaders(request);
    httpd_resp_set_type(request, "application/json");
    const esp_err_t result = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return result;
}

static bool recent(int64_t atUs, int64_t nowUs, int64_t ageUs) {
    return atUs >= 0 && nowUs >= atUs && nowUs - atUs <= ageUs;
}

static void numberOrNull(cJSON *object, const char *key, double value, bool valid) {
    if (valid && isfinite(value)) cJSON_AddNumberToObject(object, key, value);
    else cJSON_AddNullToObject(object, key);
}

static esp_err_t statusPage(httpd_req_t *request) {
    if (!authorized(request, true)) return ESP_FAIL;
    TemperatureControlStatus control = {0};
    FermentationRecipeStatus recipe = {0};
    LocalTemperature local = {0};
    IspindelReading remote = {0};
    const bool haveControl = temperature_control_latest(&control);
    const bool haveRecipe = fermentation_recipe_latest(&recipe);
    const bool haveLocal = local_temperature_latest(&local);
    const bool haveRemote = ispindel_receiver_latest(&remote);
    const int64_t now = esp_timer_get_time();
    const bool controlOk = haveControl && recent(control.updatedAtUs, now, INT64_C(2000000));
    const bool recipeOk = haveRecipe && recent(recipe.updatedAtUs, now, INT64_C(2500000));
    const bool localOk = haveLocal && local.valid && recent(local.sampledAtUs, now, INT64_C(5000000));
    const bool remoteOk = haveRemote && ispindel_reading_is_current(&remote, now);
    cJSON *root = cJSON_CreateObject();
    cJSON *sensors = root ? cJSON_AddObjectToObject(root, "sensors") : NULL;
    cJSON *controller = root ? cJSON_AddObjectToObject(root, "control") : NULL;
    cJSON *run = root ? cJSON_AddObjectToObject(root, "recipe") : NULL;
    cJSON *caps = root ? cJSON_AddObjectToObject(root, "capabilities") : NULL;
    if (!sensors || !controller || !run || !caps) {
        cJSON_Delete(root);
        return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    cJSON_AddBoolToObject(root, "uplink_connected", wifi_station_is_connected());
#if CONFIG_BEER_LOCAL_MQTT
    cJSON_AddBoolToObject(root, "cloud_configured", CONFIG_BEER_MQTT_URI[0] && CONFIG_BEER_MQTT_PASSWORD[0]);
#else
    cJSON_AddBoolToObject(root, "cloud_configured", false);
#endif
    numberOrNull(sensors, "temperature_c", local.temperatureC, localOk);
    numberOrNull(sensors, "ispindel_temperature_c", remote.packet.temperatureC, remoteOk);
    numberOrNull(sensors, "gravity", remote.packet.gravity, remoteOk);
    cJSON_AddStringToObject(sensors, "gravity_unit", remote.packet.header.gravityUnit == BREW_UNIT_SG ? "sg" :
        remote.packet.header.gravityUnit == BREW_UNIT_PLATO ? "plato" : "unknown");
    cJSON_AddBoolToObject(controller, "available", controlOk);
    numberOrNull(controller, "target_c", control.targetC, controlOk);
    cJSON_AddNumberToObject(controller, "state", control.state);
    cJSON_AddStringToObject(controller, "state_name", controlOk ? temperature_control_state_name(control.state) : "SEM RESPOSTA");
    cJSON_AddBoolToObject(controller, "enabled", controlOk && control.enabled);
    cJSON_AddBoolToObject(controller, "heater_on", controlOk && control.heaterOn);
    cJSON_AddBoolToObject(controller, "cooler_on", controlOk && control.coolerOn);
    cJSON_AddNumberToObject(controller, "wait_seconds", controlOk ? control.waitSeconds : 0);
    cJSON_AddBoolToObject(run, "available", recipeOk);
    cJSON_AddNumberToObject(run, "state", recipe.state);
    cJSON_AddStringToObject(run, "name", recipe.recipeName);
    cJSON_AddStringToObject(run, "step_name", recipe.stepName);
    cJSON_AddNumberToObject(run, "step", recipe.stepCount ? recipe.stepIndex + 1 : 0);
    cJSON_AddNumberToObject(run, "step_count", recipe.stepCount);
    cJSON_AddNumberToObject(run, "elapsed_days", recipe.elapsedDays);
    cJSON_AddNumberToObject(run, "max_days", recipe.maximumDays);
    numberOrNull(run, "gravity_limit", recipe.gravityLimit, recipe.hasGravityLimit);
    cJSON_AddNumberToObject(run, "confirmation_count", recipe.confirmationCount);
    cJSON_AddBoolToObject(run, "gravity_available", recipe.gravityAvailable);
    cJSON_AddBoolToObject(run, "storage_ok", recipe.storageOk);
    cJSON_AddNumberToObject(run, "reason", recipe.reason);
    cJSON_AddNumberToObject(run, "last_error", recipe.lastError);
    TemperatureControlSettings settings;
    temperature_control_get_settings(&settings);
    cJSON_AddNumberToObject(controller, "heating_diff_c", round(settings.heatingDifferentialC * 100.0) / 100.0);
    cJSON_AddNumberToObject(controller, "cooling_diff_c", round(settings.coolingDifferentialC * 100.0) / 100.0);
    cJSON_AddNumberToObject(caps, "target_min_c", round((settings.minimumC + settings.heatingDifferentialC) * 100.0) / 100.0);
    cJSON_AddNumberToObject(caps, "target_max_c", round((settings.maximumC - settings.coolingDifferentialC) * 100.0) / 100.0);
    cJSON_AddNumberToObject(caps, "max_steps", RECIPE_MAX_STEPS);
    cJSON_AddNumberToObject(caps, "max_json_bytes", RECIPE_JSON_MAX);
    cJSON_AddBoolToObject(caps, "uplink_configurable", wifi_station_can_configure());
    cJSON_AddBoolToObject(caps, "uplink_enterprise", wifi_station_supports_enterprise());
    if (cJSON_GetArraySize(root) != 6 || cJSON_GetArraySize(sensors) != 4 ||
        cJSON_GetArraySize(controller) != 10 || cJSON_GetArraySize(run) != 14 || cJSON_GetArraySize(caps) != 6) {
        cJSON_Delete(root);
        return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    return sendObject(request, root);
}

static esp_err_t currentRecipe(httpd_req_t *request) {
    if (!authorized(request, true)) return ESP_FAIL;
    FermentationRecipe *recipe = calloc(1, sizeof(*recipe));
    if (!recipe) return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    if (!fermentation_recipe_current(recipe)) {
        free(recipe);
        responseHeaders(request);
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_send(request, "null", HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = fermentation_recipe_json(recipe);
    free(recipe);
    if (!root) {
        return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    return sendObject(request, root);
}

static esp_err_t commandResult(httpd_req_t *request, esp_err_t result) {
    if (result != ESP_OK) {
        if (result == ESP_ERR_INVALID_ARG)
            return message(request, "400 Bad Request", "{\"error\":\"invalid_recipe\"}");
        if (result == ESP_ERR_INVALID_STATE)
            return message(request, "409 Conflict", "{\"error\":\"recipe_running_or_unavailable\"}");
        return message(request, "503 Service Unavailable", "{\"error\":\"try_again\"}");
    }
    responseHeaders(request);
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

static bool isJsonRequest(httpd_req_t *request) {
    char contentType[64] = {0};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", contentType, sizeof(contentType)) == ESP_OK &&
        !strncasecmp(contentType, "application/json", 16) &&
        (!contentType[16] || contentType[16] == ';' || contentType[16] == ' ')) return true;
    message(request, "415 Unsupported Media Type", "{\"error\":\"json_required\"}");
    return false;
}

static bool readBody(httpd_req_t *request, char *body) {
    size_t received = 0;
    const int64_t began = esp_timer_get_time();
    while (received < request->content_len) {
        const int bytes = httpd_req_recv(request, body + received, request->content_len - received);
        if (bytes <= 0 || esp_timer_get_time() - began > INT64_C(3000000)) {
            message(request, "408 Request Timeout", "{\"error\":\"incomplete_body\"}");
            return false;
        }
        received += (size_t)bytes;
    }
    return true;
}

static esp_err_t startRecipe(httpd_req_t *request) {
    if (!mutationAllowed(request) || !isJsonRequest(request)) return ESP_FAIL;
    if (!request->content_len || request->content_len > RECIPE_JSON_MAX)
        return message(request, "413 Content Too Large", "{\"error\":\"recipe_too_large\"}");
    char *body = malloc(request->content_len);
    if (!body) return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    if (!readBody(request, body)) {
        free(body);
        return ESP_FAIL;
    }
    const esp_err_t result = fermentation_recipe_submit_json(body, request->content_len);
    free(body);
    return commandResult(request, result);
}

static esp_err_t cancelRecipe(httpd_req_t *request) {
    if (!mutationAllowed(request)) return ESP_FAIL;
    if (request->content_len)
        return message(request, "400 Bad Request", "{\"error\":\"empty_body_required\"}");
    return commandResult(request, fermentation_recipe_cancel());
}

static cJSON *readSettings(httpd_req_t *request, int fields) {
    if (!mutationAllowed(request) || !isJsonRequest(request)) return NULL;

    char body[3072];
    if (!request->content_len || request->content_len >= sizeof(body)) {
        message(request, "413 Content Too Large", "{\"error\":\"invalid_settings\"}");
        return NULL;
    }
    if (!readBody(request, body)) return NULL;
    const size_t length = request->content_len;
    body[length] = 0;

    // Configuracoes sao objetos planos. Rejeita NUL, inclusive em strings JSON.
    unsigned objects = 0;
    bool quoted = false, valid = !memchr(body, 0, length);
    for (size_t i = 0; valid && i < length; ++i) {
        if (quoted && body[i] == '\\') {
            if (!strncmp(body + i + 1, "u0000", 5)) valid = false;
            ++i;
        } else if (body[i] == '"') quoted = !quoted;
        else if (!quoted && (body[i] == '[' || (body[i] == '{' && ++objects > 1))) valid = false;
    }
    cJSON *settings = valid ? cJSON_ParseWithLengthOpts(body, length + 1, NULL, true) : NULL;
    if (!cJSON_IsObject(settings) || cJSON_GetArraySize(settings) != fields) {
        cJSON_Delete(settings);
        message(request, "400 Bad Request", "{\"error\":\"invalid_settings\"}");
        return NULL;
    }
    return settings;
}

static bool getInteger(const cJSON *settings, const char *key, int minimum, int maximum, int *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings, key);
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= minimum && item->valuedouble <= maximum) ||
        floor(item->valuedouble) != item->valuedouble) return false;
    *value = (int)item->valuedouble;
    return true;
}

static esp_err_t settingsResult(httpd_req_t *request, esp_err_t result, const char *invalid) {
    if (result == ESP_ERR_INVALID_ARG) return message(request, "400 Bad Request", invalid);
    if (result != ESP_OK) return message(request, "503 Service Unavailable", "{\"error\":\"settings_unavailable\"}");
    responseHeaders(request);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, "{\"saved\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t saveUplink(httpd_req_t *request) {
    cJSON *settings = readSettings(request, 4);
    if (!settings) return ESP_FAIL;
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "ssid"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "password"));
    const char *username = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "username"));
    const char *identity = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(settings, "identity"));
    const esp_err_t result = ssid && password && username && identity
        ? wifi_station_set_uplink(ssid, password, username, identity) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(settings);
    return settingsResult(request, result, "{\"error\":\"invalid_uplink\"}");
}

static esp_err_t saveHysteresis(httpd_req_t *request) {
    cJSON *settings = readSettings(request, 2);
    if (!settings) return ESP_FAIL;
    int heating, cooling;
    const bool valid = getInteger(settings, "heating_centic", 10, 500, &heating) &&
        getInteger(settings, "cooling_centic", 10, 500, &cooling);
    cJSON_Delete(settings);
    const esp_err_t result = valid ? fermentation_recipe_set_hysteresis(heating, cooling) : ESP_ERR_INVALID_ARG;
    return settingsResult(request, result, "{\"error\":\"invalid_hysteresis\"}");
}

static esp_err_t skipRecipeStep(httpd_req_t *request) {
    cJSON *settings = readSettings(request, 1);
    if (!settings) return ESP_FAIL;
    int step;
    const bool valid = getInteger(settings, "step", 1, RECIPE_MAX_STEPS, &step);
    cJSON_Delete(settings);
    if (!valid) return message(request, "400 Bad Request", "{\"error\":\"invalid_settings\"}");
    const esp_err_t result = fermentation_recipe_skip_step(step - 1);
    if (result == ESP_ERR_INVALID_STATE)
        return message(request, "409 Conflict", "{\"error\":\"step_changed\"}");
    return commandResult(request, result);
}

static esp_err_t indexPage(httpd_req_t *request) {
#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
    if (!hostValid(request)) return redirectToPortal(request);
#endif
    if (!authorized(request, false)) return ESP_FAIL;
    extern const uint8_t indexStart[] __asm__("_binary_index_html_start");
    responseHeaders(request);
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; "
        "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, (const char *)indexStart, HTTPD_RESP_USE_STRLEN);
}

esp_err_t local_web_ui_register(httpd_handle_t server, bool (*localClient)(httpd_req_t *)) {
    isLocalClient = localClient;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = indexPage},
        {.uri = "/api/status", .method = HTTP_GET, .handler = statusPage},
        {.uri = "/api/recipe", .method = HTTP_GET, .handler = currentRecipe},
        {.uri = "/api/recipe", .method = HTTP_POST, .handler = startRecipe},
        {.uri = "/api/recipe/cancel", .method = HTTP_POST, .handler = cancelRecipe},
        {.uri = "/api/recipe/skip", .method = HTTP_POST, .handler = skipRecipeStep},
        {.uri = "/api/settings/wifi", .method = HTTP_POST, .handler = saveUplink},
        {.uri = "/api/settings/hysteresis", .method = HTTP_POST, .handler = saveHysteresis},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const esp_err_t result = httpd_register_uri_handler(server, &routes[i]);
        if (result != ESP_OK) return result;
    }
#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captiveNotFound);
#else
    return ESP_OK;
#endif
}
