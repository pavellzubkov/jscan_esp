idf_component_register(
    SRCS "src/BootManager.cpp"
         "src/J1939Proto.cpp"
         "src/FieldRegistry.cpp"
    INCLUDE_DIRS "include"
                 "include/fields"
    REQUIRES esp_event freertos log
)
