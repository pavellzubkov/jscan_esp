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