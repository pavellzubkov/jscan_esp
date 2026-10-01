idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES common driver esp_driver_twai esp_driver_gpio esp_common freertos log
)
