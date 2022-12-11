idf_component_register(
    SRCS app_common.cpp
    INCLUDE_DIRS .
    REQUIRES esp_wifi esp_http_server json vfs
)