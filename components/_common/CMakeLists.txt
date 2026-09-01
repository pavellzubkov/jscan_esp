idf_component_register(
    SRCS app_common.cpp
    INCLUDE_DIRS .
    REQUIRES esp_http_server esp_driver_ledc esp_netif espressif__cjson
)