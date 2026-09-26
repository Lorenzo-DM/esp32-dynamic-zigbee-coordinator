# Host-side unit tests for the portable config parsing logic.
# Builds without the ESP-IDF: run with
#   cmake -S test/host -B build_host && cmake --build build_host && ctest --test-dir build_host
cmake_minimum_required(VERSION 3.16)
project(config_parser_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(MAIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../main)

add_executable(test_config_parser
    test_config_parser.c
    ${MAIN_DIR}/config_parser.c
)

target_include_directories(test_config_parser PRIVATE ${MAIN_DIR})
target_compile_definitions(test_config_parser PRIVATE HOST_TEST)
target_compile_options(test_config_parser PRIVATE -Wall -Wextra)

add_test(NAME config_parser COMMAND test_config_parser)
