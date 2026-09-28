cmake_minimum_required(VERSION 3.16)
project(host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host-тесты без IDF: собираются нативно (Linux/macOS/Windows+MinGW/WSL).
# Сборка: cmake -S tests/host -B build/host && cmake --build build/host
# Запуск: ./build/host/host_tests

set(HOST_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(COMMON_DIR   ${HOST_TEST_DIR}/../../components/01_core/common)
set(J1939_DIR    ${HOST_TEST_DIR}/../../components/03_systems/j1939_system)

add_executable(host_tests
    main.cpp
    ${COMMON_DIR}/src/J1939Proto.cpp
    ${COMMON_DIR}/src/FieldRegistry.cpp
    ${J1939_DIR}/src/SnapshotAccumulator.cpp
)

target_include_directories(host_tests PRIVATE
    ${HOST_TEST_DIR}/stubs          # esp_log.h / driver/gpio.h — заглушки вместо IDF
    ${COMMON_DIR}/include
    ${COMMON_DIR}/include/fields
    ${J1939_DIR}/include
)

target_compile_definitions(host_tests PRIVATE HOST_TEST=1)