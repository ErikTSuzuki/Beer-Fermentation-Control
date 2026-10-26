"""Exercise the real local HTTP handlers with host HTTP/sensor/task doubles.

Run: python3 tests/test_local_web_ui.py [path-to-gcc]
Generated artifacts stay in build/local-ui-tests; no board or network is used.
"""
from pathlib import Path
import shutil
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT / "build/local-ui-tests"
CJSON = PROJECT / "managed_components/espressif__cjson/cJSON"

MOCK = r'''
#ifndef LOCAL_UI_MOCK_H
#define LOCAL_UI_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef void *httpd_handle_t;
typedef struct { size_t content_len; int method; const char *uri; } httpd_req_t;
typedef int httpd_err_code_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define HTTP_GET 1
#define HTTP_POST 2
#define HTTP_HEAD 3
#define HTTPD_404_NOT_FOUND 4
#define HTTPD_RESP_USE_STRLEN -1
#define CONFIG_BEER_AP_IP "192.168.50.1"
#define CONFIG_BEER_AP_PASSWORD "test-password"
#ifndef CONFIG_BEER_LOCAL_UI_PASSWORD
#define CONFIG_BEER_LOCAL_UI_PASSWORD ""
#endif
#define CONFIG_BEER_ISPINDEL_STALE_SECONDS 1800
#define configTICK_RATE_HZ 1000
int httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
int httpd_register_err_handler(httpd_handle_t, httpd_err_code_t, esp_err_t (*)(httpd_req_t *, httpd_err_code_t));
int httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
int httpd_resp_set_type(httpd_req_t *, const char *);
int httpd_resp_set_status(httpd_req_t *, const char *);
int httpd_resp_send(httpd_req_t *, const char *, int);
int httpd_req_recv(httpd_req_t *, char *, size_t);
int64_t esp_timer_get_time(void);
#endif
'''

HARNESS = r'''
#include "LocalWebUi.c"
#include <assert.h>
#include <stdio.h>

#define CHECK(x) do { assert(x); ++checks; } while (0)
const uint8_t html[] __asm__("_binary_index_html_start")="<!doctype html><title>Local</title>";
static httpd_uri_t registered[5];
static esp_err_t (*notFound)(httpd_req_t *, httpd_err_code_t);
static unsigned routeCount, checks, submitted, cancelled;
static bool localClient=true, haveProfile=true, stale, disconnected, slow, readFail;
static const char *host=CONFIG_BEER_AP_IP, *origin="http://" CONFIG_BEER_AP_IP;
static const char *access, *contentType="application/json", *input="{}";
static char output[8192], responseType[64];
static size_t offset;
static int responseStatus;
static int64_t now=INT64_C(10000000);
static esp_err_t submitResult=ESP_OK;
static bool noStore, closed, csp, redirected;
static long failAllocation=-1;
static FermentationRecipe storedProfile;

static bool local(httpd_req_t *req) { (void)req; return localClient; }
int64_t esp_timer_get_time(void) { return now; }
int httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route) {
    CHECK(server==(void *)1 && routeCount<5); registered[routeCount++]=*route; return ESP_OK;
}
int httpd_register_err_handler(httpd_handle_t server, httpd_err_code_t error,
    esp_err_t (*handler)(httpd_req_t *, httpd_err_code_t)) {
    CHECK(server==(void *)1 && error==HTTPD_404_NOT_FOUND);notFound=handler;return ESP_OK;
}
int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *out, size_t size) {
    (void)req;
    const char *value=!strcmp(name,"Host")?host:!strcmp(name,"Origin")?origin:
        !strcmp(name,"X-Local-Key")?access:!strcmp(name,"Content-Type")?contentType:NULL;
    if (!value || strlen(value)>=size) return ESP_FAIL;
    strcpy(out,value); return ESP_OK;
}
int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value) {
    (void)req;
    if (!strcmp(key,"Cache-Control")) noStore=!strcmp(value,"no-store");
    if (!strcmp(key,"Connection")) closed=!strcmp(value,"close");
    if (!strcmp(key,"Content-Security-Policy")) csp=strstr(value,"frame-ancestors 'none'")!=NULL;
    if (!strcmp(key,"Location")) {CHECK(!strcmp(value,"http://" CONFIG_BEER_AP_IP "/"));redirected=true;}
    return ESP_OK;
}
int httpd_resp_set_type(httpd_req_t *req, const char *value) { (void)req; strcpy(responseType,value); return ESP_OK; }
int httpd_resp_set_status(httpd_req_t *req, const char *value) { (void)req; responseStatus=atoi(value); return ESP_OK; }
int httpd_resp_send(httpd_req_t *req, const char *body, int size) {
    (void)req; size_t length=size<0?strlen(body):(size_t)size; CHECK(length<sizeof(output));
    memcpy(output,body,length); output[length]='\0'; return ESP_OK;
}
int httpd_req_recv(httpd_req_t *req, char *out, size_t size) {
    (void)req; if (readFail) return -1;
    if (size>7) size=7;
    memcpy(out,input+offset,size); offset+=size; now+=slow?4000000:1; return (int)size;
}
bool temperature_control_latest(TemperatureControlStatus *out) {
    *out=(TemperatureControlStatus){.targetC=20,.state=CONTROL_COOLING,.coolerOn=true,.enabled=true,
        .sensorValid=true,.updatedAtUs=stale?now-3000000:now}; return true;
}
const char *temperature_control_state_name(TemperatureControlState state) { (void)state; return "RESFRIANDO"; }
void temperature_control_get_settings(TemperatureControlSettings *out) {
    *out=(TemperatureControlSettings){.minimumC=-5,.maximumC=40,.heatingDifferentialC=.5f,.coolingDifferentialC=.5f};
}
bool fermentation_recipe_latest(FermentationRecipeStatus *out) {
    *out=(FermentationRecipeStatus){.state=RECIPE_RUNNING,.stepIndex=1,.stepCount=3,
        .elapsedDays=2.5,.maximumDays=5,.hasGravityLimit=true,.gravityLimit=1.01f,.storageOk=true,
        .updatedAtUs=stale?now-3000000:now};
    strcpy(out->recipeName,"Receita \"teste\"\\"); strcpy(out->stepName,"Etapa 2"); return true;
}
bool fermentation_recipe_current(FermentationRecipe *out) { *out=storedProfile; return haveProfile; }
esp_err_t fermentation_recipe_submit_json(const char *json, size_t length) {
    ++submitted;
    FermentationRecipe profile;
    if (!fermentation_recipe_parse(json,length,&profile)) return ESP_ERR_INVALID_ARG;
    return submitResult;
}
esp_err_t fermentation_recipe_cancel(void) { ++cancelled; return submitResult; }
bool local_temperature_latest(LocalTemperature *out) {
    *out=(LocalTemperature){.valid=true,.temperatureC=20.25f,.sampledAtUs=stale?now-6000000:now}; return true;
}
bool ispindel_receiver_latest(IspindelReading *out) {
    *out=(IspindelReading){.receivedAtUs=stale?-1:now,.packet={.temperatureC=20.1f,.gravity=1.018f,
        .header={.gravityUnit=BREW_UNIT_SG}}}; return true;
}
bool wifi_station_is_connected(void) { return !disconnected; }
static cJSON *response(void) { cJSON *value=cJSON_Parse(output); CHECK(value!=NULL); return value; }
static int call(const char *uri, int method, const char *body, size_t length) {
    input=body; offset=0; responseStatus=200; output[0]=0; noStore=closed=csp=redirected=false;
    httpd_req_t request={.content_len=length,.method=method,.uri=uri};
    for (unsigned i=0; i<routeCount; ++i) {
        if (registered[i].method==method && !strcmp(registered[i].uri,uri)) {
            registered[i].handler(&request); CHECK(noStore&&closed); return responseStatus;
        }
    }
    CHECK(notFound!=NULL);notFound(&request,HTTPD_404_NOT_FOUND);CHECK(noStore&&closed);return responseStatus;
}
static void *controlledMalloc(size_t size) {
    if (failAllocation==0) return NULL;
    if (failAllocation>0) --failAllocation;
    return malloc(size);
}
static void statusAssertions(void) {
    cJSON *root=response(), *control=cJSON_GetObjectItemCaseSensitive(root,"control"),
        *recipe=cJSON_GetObjectItemCaseSensitive(root,"recipe"), *sensors=cJSON_GetObjectItemCaseSensitive(root,"sensors");
    CHECK(cJSON_GetArraySize(root)==6 && cJSON_GetArraySize(recipe)==14);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(control,"available"))==!stale);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(sensors,"temperature_c"))==stale);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(sensors,"gravity"))==stale);
    CHECK(!strcmp(cJSON_GetObjectItem(recipe,"name")->valuestring,"Receita \"teste\"\\"));
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"capabilities"),"target_min_c")->valuedouble==-4.5);
    cJSON_Delete(root);
}
int main(int argc, char **argv) {
    CHECK(argc==2); FILE *file=fopen(argv[1],"rb"); CHECK(file!=NULL);
    char profile[4097]; size_t length=fread(profile,1,4096,file); fclose(file); profile[length]=0;
    CHECK(fermentation_recipe_parse(profile,length,&storedProfile));
    const char *expected=CONFIG_BEER_LOCAL_UI_PASSWORD[0]?CONFIG_BEER_LOCAL_UI_PASSWORD:CONFIG_BEER_AP_PASSWORD;
    char encoded[129]={0};
    for(size_t i=0;i<strlen(expected);++i) snprintf(encoded+i*2,3,"%02x",(unsigned char)expected[i]);
    access=encoded;
    CHECK(local_web_ui_register((void *)1,local)==ESP_OK && routeCount==5);
    CHECK(call("/",HTTP_GET,"",0)==200 && csp && strstr(output,"doctype"));
    CHECK(!strcmp(responseType,"text/html; charset=utf-8"));
    CHECK(call("/api/status",HTTP_GET,"",0)==200); statusAssertions();
    access="wrong-password"; CHECK(call("/api/status",HTTP_GET,"",0)==401);
    CHECK(call("/api/recipe",HTTP_POST,profile,length)==401 && submitted==0);
    access=NULL; CHECK(call("/api/recipe/cancel",HTTP_POST,"",0)==401 && cancelled==0);
    if (CONFIG_BEER_LOCAL_UI_PASSWORD[0]) {
        char apEncoded[129]={0};
        for(size_t i=0;i<strlen(CONFIG_BEER_AP_PASSWORD);++i)
            snprintf(apEncoded+i*2,3,"%02x",(unsigned char)CONFIG_BEER_AP_PASSWORD[i]);
        access=apEncoded;CHECK(call("/api/status",HTTP_GET,"",0)==401);
    }
    access=encoded; localClient=false;
    CHECK(call("/",HTTP_GET,"",0)==403);CHECK(call("/api/recipe",HTTP_POST,profile,length)==403 && submitted==0);
    localClient=true;host="evil.example";
#if CONFIG_BEER_CAPTIVE_PORTAL_ENABLED
    CHECK(call("/",HTTP_GET,"",0)==302 && redirected);
    // Probe hosts receive only a canonical redirect, never UI/API credentials.
    CHECK(call("/api/status",HTTP_GET,"",0)==403);
    CHECK(call("/api/recipe",HTTP_POST,profile,length)==403 && submitted==0);
    access=NULL;
    const char *probes[]={"/generate_204","/hotspot-detect.html","/connecttest.txt","/ncsi.txt","/canonical.html"};
    for(size_t i=0;i<sizeof(probes)/sizeof(probes[0]);++i){
        CHECK(call(probes[i],HTTP_GET,"",0)==302 && redirected && strstr(output,"http://192.168.50.1/"));
    }
    CHECK(call("/generate_204",HTTP_HEAD,"",0)==302 && redirected && !output[0]);
    CHECK(call("/generate_204",HTTP_POST,"",0)==403 && !redirected);
    CHECK(call("/api/missing",HTTP_GET,"",0)==404 && !redirected);
    localClient=false;CHECK(call("/generate_204",HTTP_GET,"",0)==403 && !redirected);localClient=true;
    access=encoded;
#else
    CHECK(call("/",HTTP_GET,"",0)==403);
#endif
    host=CONFIG_BEER_AP_IP ":80";CHECK(call("/",HTTP_GET,"",0)==200);host=CONFIG_BEER_AP_IP;
    origin="http://evil.example";CHECK(call("/api/recipe",HTTP_POST,profile,length)==403 && submitted==0);
    origin=NULL;CHECK(call("/api/recipe/cancel",HTTP_POST,"",0)==403 && cancelled==0);
    origin="http://" CONFIG_BEER_AP_IP;contentType="text/plain";CHECK(call("/api/recipe",HTTP_POST,profile,length)==415);
    contentType="application/json-malformed";CHECK(call("/api/recipe",HTTP_POST,profile,length)==415);
    contentType="Application/JSON; charset=utf-8";CHECK(call("/api/recipe",HTTP_POST,profile,length)==202 && submitted==1);
    CHECK(call("/api/recipe",HTTP_POST,profile,0)==413);CHECK(call("/api/recipe",HTTP_POST,profile,4097)==413);
    CHECK(call("/api/recipe",HTTP_POST,"{}",2)==400 && submitted==2);
    readFail=true;CHECK(call("/api/recipe",HTTP_POST,profile,length)==408);readFail=false;
    slow=true;CHECK(call("/api/recipe",HTTP_POST,profile,length)==408);slow=false;
    submitResult=ESP_ERR_INVALID_STATE;CHECK(call("/api/recipe",HTTP_POST,profile,length)==409);
    submitResult=ESP_ERR_TIMEOUT;CHECK(call("/api/recipe/cancel",HTTP_POST,"",0)==503);
    submitResult=ESP_OK;CHECK(call("/api/recipe/cancel",HTTP_POST,"x",1)==400);
    CHECK(call("/api/recipe/cancel",HTTP_POST,"",0)==202);
    CHECK(call("/api/recipe",HTTP_GET,"",0)==200);
    cJSON *root=response(); CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root,"steps"))==3);cJSON_Delete(root);
    FermentationRecipe roundTrip;CHECK(fermentation_recipe_parse(output,strlen(output),&roundTrip));
    CHECK(roundTrip.steps[0].gravityLimit==storedProfile.steps[0].gravityLimit);
    haveProfile=false;CHECK(call("/api/recipe",HTTP_GET,"",0)==200&&!strcmp(output,"null"));haveProfile=true;
    stale=true;CHECK(call("/api/status",HTTP_GET,"",0)==200);statusAssertions();stale=false;
    // Allocation failures cannot report an incomplete status/profile as success.
    for (int i=0;i<180;++i) {
        cJSON_Hooks hooks={.malloc_fn=controlledMalloc,.free_fn=free};
        failAllocation=i;cJSON_InitHooks(&hooks);int code=call("/api/status",HTTP_GET,"",0);cJSON_InitHooks(NULL);
        CHECK(code==503||code==200);if(code==200)statusAssertions();
        failAllocation=i;cJSON_InitHooks(&hooks);code=call("/api/recipe",HTTP_GET,"",0);cJSON_InitHooks(NULL);
        CHECK(code==503||code==200);if(code==200){CHECK(fermentation_recipe_parse(output,strlen(output),&roundTrip));}
    }
    printf("%u local HTTP checks passed (auth, origin, limits, commands, status, JSON and memory failures)\n",checks);
}
'''


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else shutil.which("gcc")
    if not compiler:
        raise SystemExit("gcc required")
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "local_ui_mock.h").write_text(MOCK, encoding="utf-8")
    for name in ["esp_err.h", "esp_http_server.h", "esp_timer.h", "sdkconfig.h", "freertos/FreeRTOS.h"]:
        path = BUILD / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "local_ui_mock.h"\n', encoding="utf-8")
    source = BUILD / "local_ui_test.c"
    source.write_text(HARNESS, encoding="utf-8")
    for index, (password, portal) in enumerate((("",0),("local-only-password",0),("senha-ação",0),
                                              ("",1),("local-only-password",1),("senha-ação",1))):
        executable = BUILD / f"local_ui_test_{index}"
        flags = [f'-DCONFIG_BEER_LOCAL_UI_PASSWORD="{password}"'] if password else []
        flags.append(f'-DCONFIG_BEER_CAPTIVE_PORTAL_ENABLED={portal}')
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", *flags,
            "-DCJSON_NESTING_LIMIT=8", "-I" + str(BUILD), "-I" + str(PROJECT / "main"), "-I" + str(CJSON),
            str(source), str(PROJECT / "main/FermentationRecipeJson.c"), str(PROJECT / "main/FermentationRecipeLogic.c"),
            str(CJSON / "cJSON.c"), "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), str(PROJECT / "recipes/exemplo.json")], check=True, timeout=15)


if __name__ == "__main__":
    main()
