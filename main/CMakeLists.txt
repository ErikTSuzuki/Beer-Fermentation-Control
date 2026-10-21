idf_component_register(
    SRCS
        "main.c"
        "WifiStation.c"
        "IspindelReceiver.c"
        "LocalTemperature.c"
        "TelemetryLogger.c"
        "Mar2406Display.c"
        "DisplayTest.c"
        "RtcClock.c"
        "RelayTest.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash onewire_bus ds18b20 esp_driver_gpio esp_driver_i2c esp_rom
)

# Os fontes da aplicacao usam C11 ou superior.
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
