idf_component_register(
    SRCS
        "main.cpp"
        "WifiStation.cpp"
        "IspindelReceiver.cpp"
        "LocalTemperature.cpp"
        "TelemetryLogger.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash onewire_bus ds18b20
)
