idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common littlefs_service esp_http_server vfs heap freertos log
             cjson app_update esp_partition esp_system esp_driver_gpio
)