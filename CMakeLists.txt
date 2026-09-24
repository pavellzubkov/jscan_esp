cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components/01_core
    ${CMAKE_CURRENT_LIST_DIR}/components/02_hardware
    ${CMAKE_CURRENT_LIST_DIR}/components/03_systems
    ${CMAKE_CURRENT_LIST_DIR}/components/04_network
    ${CMAKE_CURRENT_LIST_DIR}/components/05_storage
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JScaner)

# Автоматическая упаковка OTA-архива после успешной сборки
idf_build_get_property(build_dir BUILD_DIR)
add_custom_command(
    TARGET app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E env python ${CMAKE_CURRENT_LIST_DIR}/scripts/package_ota.py ${build_dir}
    COMMENT "Packaging OTA update archive (update.zip)"
)