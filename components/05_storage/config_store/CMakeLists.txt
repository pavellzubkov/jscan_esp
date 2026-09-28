idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common littlefs_service espressif__cjson esp_system freertos log nvs_flash
)