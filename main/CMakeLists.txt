idf_component_register(
    SRCS
        "main.c"
        "WifiStation.c"
        "UbidotsTelemetry.c"
        "IspindelReceiver.c"
        "BrewJson.c"
        "LocalTemperature.c"
        "TelemetryLogger.c"
        "Mar2406Display.c"
        "DisplayTest.c"
        "RtcClock.c"
        "TemperatureControl.c"
        "TemperatureControlLogic.c"
        "FermentationRecipe.c"
        "FermentationRecipeLogic.c"
        "FermentationRecipeJson.c"
        "LocalWebUi.c"
        "CaptivePortal.c"
        "CaptiveDns.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash onewire_bus ds18b20 esp_driver_gpio esp_driver_i2c esp_rom
    PRIV_REQUIRES mqtt wpa_supplicant lwip esp_http_server cjson esp_timer
    EMBED_TXTFILES "../recipes/exemplo.json" "web/index.html"
)

# Os fontes da aplicacao usam C11 ou superior.
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
