cmake_minimum_required(VERSION 3.22)
project(VehicleTelemetryLab LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LAB_WERROR "Treat compiler warnings as errors" ON)
option(LAB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# Crash points (LAB_CRASH_AT) exist only in test builds; Release leaves them out.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    option(LAB_CRASH_HOOKS "Compile test crash points into the collector" OFF)
else()
    option(LAB_CRASH_HOOKS "Compile test crash points into the collector" ON)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
if(LAB_WERROR)
    add_compile_options(-Werror)
endif()
if(LAB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# Store library and its original command-line tool.
add_library(metrics_store
    src/FileOps.cpp
    src/MemStore.cpp
    src/MetricsStore.cpp
    src/Record.cpp
    src/WriteAheadLog.cpp)
target_include_directories(metrics_store PUBLIC src)

add_executable(metrics_cli src/Cli.cpp)
target_link_libraries(metrics_cli PRIVATE metrics_store)

# Lighting controller: pure logic plus the SocketCAN program.
add_library(lamp_controller
    controller/frame_codec.cpp
    controller/lighting_controller.cpp
    controller/controller_node.cpp)
target_include_directories(lamp_controller PUBLIC controller)

add_executable(lighting_controller_can controller/socketcan_main.cpp)
target_link_libraries(lighting_controller_can PRIVATE lamp_controller)

add_executable(frame_tool controller/frame_tool.cpp)
target_link_libraries(frame_tool PRIVATE lamp_controller)

# MQTT collector.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)
find_package(nlohmann_json 3.11 REQUIRED)

add_library(telemetry_message collector/telemetry_message.cpp)
target_include_directories(telemetry_message PUBLIC collector)
target_link_libraries(telemetry_message PUBLIC metrics_store nlohmann_json::nlohmann_json)

add_executable(mqtt_collector collector/mqtt_collector.cpp)
target_link_libraries(mqtt_collector PRIVATE telemetry_message PkgConfig::MOSQUITTO)
if(LAB_CRASH_HOOKS)
    target_compile_definitions(mqtt_collector PRIVATE LAB_CRASH_HOOKS)
endif()

# Tests. GoogleTest checks stay active in Release builds, unlike assert().
enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(store_tests tests/MetricsStoreTests.cpp tests/CheckedStoreTests.cpp)
target_link_libraries(store_tests PRIVATE metrics_store GTest::gtest_main)
target_compile_definitions(store_tests PRIVATE LAB_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(store_tests)

add_executable(controller_tests tests/ControllerTests.cpp)
target_link_libraries(controller_tests PRIVATE lamp_controller nlohmann_json::nlohmann_json GTest::gtest_main)
target_compile_definitions(controller_tests PRIVATE LAB_EXPECTED_DIR="${CMAKE_SOURCE_DIR}/tests/expected")
gtest_discover_tests(controller_tests)

add_executable(collector_tests tests/CollectorMessageTests.cpp)
target_link_libraries(collector_tests PRIVATE telemetry_message GTest::gtest_main)
gtest_discover_tests(collector_tests)

add_executable(harness_tests tests/harness_tests.cpp)
target_link_libraries(harness_tests PRIVATE GTest::gtest_main)
gtest_discover_tests(harness_tests)
