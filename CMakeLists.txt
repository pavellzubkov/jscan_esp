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

# Автоматическая упаковка OTA-архива после успешной сборки.
# OUTPUT+custom_target (вместо голого POST_BUILD): так задаётся DEPENDS от
# скрипта упаковки — правка scripts/package_ota.py пересобирает update.zip.
idf_build_get_property(build_dir BUILD_DIR)
set(OTA_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/scripts/package_ota.py)
set(OTA_ZIP ${build_dir}/update.zip)
add_custom_command(
    OUTPUT ${OTA_ZIP}
    COMMAND ${CMAKE_COMMAND} -E env python ${OTA_SCRIPT} ${build_dir}
    COMMENT "Packaging OTA update archive (update.zip)"
    DEPENDS ${OTA_SCRIPT} app
    VERBATIM
)
add_custom_target(package_ota ALL DEPENDS ${OTA_ZIP})