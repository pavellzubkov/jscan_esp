idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common spiffs_service esp_http_server vfs heap freertos log
)