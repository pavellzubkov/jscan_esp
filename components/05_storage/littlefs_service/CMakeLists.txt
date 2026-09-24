idf_component_register(
    SRCS "src/LittleFsService.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_littlefs
        vfs
        log
        freertos
)