idf_component_register(
    SRCS app_common.cpp
    INCLUDE_DIRS .
    REQUIRES esp_http_server json esp_driver_ledc driver esp_netif
)