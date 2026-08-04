## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## ESP-IDF project entry point. NOT BUILT YET. See README.md in this directory.
##
## This file is the documented shape of the device build, kept next to the code
## it will build so that the repository layout stays compatible with ESP-IDF's
## component rules from the first commit. It is not expected to work until
## Phase 1b, and the HAL backends it needs do not exist.
##
## When it is time:
##     cd ports/esp32
##     idf.py set-target esp32s3
##     idf.py build flash monitor

cmake_minimum_required(VERSION 3.20)

# Point ESP-IDF at the shared module directories. hakoniwaos/CMakeLists.txt
# detects ESP_PLATFORM and registers itself as a component instead of an
# ordinary static library, reading the same sources.cmake the desktop build
# uses.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../hakoniwaos"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(kamiframe-firmware)
