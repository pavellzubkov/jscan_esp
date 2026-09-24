idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common esp_wifi esp_netif esp_event esp_driver_gpio freertos lwip log
)