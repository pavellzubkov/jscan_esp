idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ""
    REQUIRES common config_store communication netctrl j1939_system
             freertos log
)

# Create a LittleFS image from the contents of the 'frontend/dist' directory
# that fits the partition named 'storage'. FLASH_IN_PROJECT indicates that
# the generated image should be flashed when the entire project is flashed to
# the target with 'idf.py -p PORT flash'.

littlefs_create_partition_image(storage ../frontend/dist FLASH_IN_PROJECT)