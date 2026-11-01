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
    const bool remoteOk = haveRemote && recent(remote.receivedAtUs, now,
        (int64_t)CONFIG_BEER_ISPINDEL_STALE_SECONDS * 1000000);
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
#if CONFIG_BEER_UBIDOTS_ENABLED
    cJSON_AddBoolToObject(root, "cloud_configured", CONFIG_BEER_UBIDOTS_TOKEN[0] && CONFIG_BEER_WIFI_SSID[0]);
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
    cJSON_AddNumberToObject(caps, "target_min_c", settings.minimumC + settings.heatingDifferentialC);
    cJSON_AddNumberToObject(caps, "target_max_c", settings.maximumC - settings.coolingDifferentialC);
    cJSON_AddNumberToObject(caps, "max_steps", RECIPE_MAX_STEPS);
    cJSON_AddNumberToObject(caps, "max_json_bytes", RECIPE_JSON_MAX);
    if (cJSON_GetArraySize(root) != 6 || cJSON_GetArraySize(sensors) != 4 ||
        cJSON_GetArraySize(controller) != 8 || cJSON_GetArraySize(run) != 14 || cJSON_GetArraySize(caps) != 4) {
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
    cJSON *root = cJSON_CreateObject();
    cJSON *steps = root ? cJSON_AddArrayToObject(root, "steps") : NULL;
    if (!steps) {
        free(recipe); cJSON_Delete(root);
        return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    }
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "name", recipe->name);
    cJSON_AddStringToObject(root, "gravity_unit", recipe->gravityUnit == BREW_UNIT_SG ? "sg" : "plato");
    cJSON_AddNumberToObject(root, "gravity_confirm_samples", recipe->confirmationSamples);
    cJSON_AddStringToObject(root, "on_complete", "hold");
    for (unsigned i = 0; i < recipe->stepCount; ++i) {
        const FermentationStep *step = &recipe->steps[i];
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(steps, item)) {
            cJSON_Delete(item); cJSON_Delete(root); free(recipe);
            return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
        }
        cJSON_AddStringToObject(item, "name", step->name);
        cJSON_AddNumberToObject(item, "target_c", round(step->targetC * 1000000.0) / 1000000.0);
        cJSON_AddNumberToObject(item, "max_days", (double)step->maximumUs / RECIPE_DAY_US);
        numberOrNull(item, "gravity_limit", round(step->gravityLimit * 1000000.0) / 1000000.0, step->hasGravityLimit);
        if (cJSON_GetArraySize(item) != 4) {
            cJSON_Delete(root); free(recipe);
            return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
        }
    }
    free(recipe);
    if (cJSON_GetArraySize(root) != 6) {
        cJSON_Delete(root);
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

static esp_err_t startRecipe(httpd_req_t *request) {
    if (!mutationAllowed(request)) return ESP_FAIL;
    char contentType[64] = {0};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", contentType, sizeof(contentType)) != ESP_OK ||
        strncasecmp(contentType, "application/json", 16) ||
        (contentType[16] && contentType[16] != ';' && contentType[16] != ' '))
        return message(request, "415 Unsupported Media Type", "{\"error\":\"json_required\"}");
    if (!request->content_len || request->content_len > RECIPE_JSON_MAX)
        return message(request, "413 Content Too Large", "{\"error\":\"recipe_too_large\"}");
    char *body = malloc(request->content_len);
    if (!body) return message(request, "503 Service Unavailable", "{\"error\":\"no_memory\"}");
    size_t received = 0;
    const int64_t began = esp_timer_get_time();
    while (received < request->content_len) {
        const int bytes = httpd_req_recv(request, body + received, request->content_len - received);
        if (bytes <= 0 || esp_timer_get_time() - began > INT64_C(3000000)) {
            free(body);
            return message(request, "408 Request Timeout", "{\"error\":\"incomplete_body\"}");
        }
        received += (size_t)bytes;
    }
    const esp_err_t result = fermentation_recipe_submit_json(body, received);
    free(body);
    return commandResult(request, result);
}

static esp_err_t cancelRecipe(httpd_req_t *request) {
    if (!mutationAllowed(request)) return ESP_FAIL;
    if (request->content_len)
        return message(request, "400 Bad Request", "{\"error\":\"empty_body_required\"}");
    return commandResult(request, fermentation_recipe_cancel());
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
