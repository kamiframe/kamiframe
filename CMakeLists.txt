## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Desktop / host build entry point.
##
## The ESP-IDF build does NOT start here. It starts at ports/esp32/, which
## points EXTRA_COMPONENT_DIRS back at the same module directories. Each module
## has one CMakeLists.txt that branches on ESP_PLATFORM, so the source lists
## exist exactly once and cannot drift.
##
## Quick start:
##     cmake -B build
##     cmake --build build
##     ./build/simulator/kamiframe-sim

cmake_minimum_required(VERSION 3.20)

project(kamiframe
        VERSION 0.1.0
        DESCRIPTION "Kamiframe: open source virtual pet platform"
        LANGUAGES C CXX)

if(ESP_PLATFORM)
    message(FATAL_ERROR
        "This is the host build. For the ESP32 build, run idf.py from "
        "ports/esp32/ instead.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(KAMIFRAME_BUILD_SIMULATOR "Build the SDL3 simulator" ON)
option(KAMIFRAME_BUILD_HEADLESS  "Build the headless CI runner" ON)
option(KAMIFRAME_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(kamiframe_options)

# Must be here, at the top level, or ctest run from the build root finds
# nothing.
include(CTest)
enable_testing()

add_subdirectory(hakoniwaos)
add_subdirectory(simulator)

message(STATUS "")
message(STATUS "Kamiframe ${PROJECT_VERSION}")
message(STATUS "  build type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++            : ${CMAKE_CXX_STANDARD} (no exceptions, no RTTI)")
message(STATUS "  simulator (SDL): ${KAMIFRAME_BUILD_SIMULATOR}")
message(STATUS "  headless       : ${KAMIFRAME_BUILD_HEADLESS}")
message(STATUS "")
