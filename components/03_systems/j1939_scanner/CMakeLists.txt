idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common freertos esp_event esp_timer log esp_common
)