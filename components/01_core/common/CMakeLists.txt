idf_component_register(
    SRCS "src/BootManager.cpp"
         "src/J1939Proto.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event freertos log
)
