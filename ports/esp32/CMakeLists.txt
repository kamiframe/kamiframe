## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## ESP-IDF project entry point. Builds a plain hello-world (ADR 0019) --
## Phase 1's second named hardware exit trigger, see README.md in this
## directory. The ESP32 HAL backends kf_app_frame() needs still do not
## exist; that is Phase 1b, which this slice's own README section explains
## starts only after this hello-world boots.
##
##     cd ports/esp32
##     idf.py set-target esp32s3
##     idf.py build
##     idf.py flash monitor         # needs real hardware
##     idf.py wokwi                 # needs a WOKWI_CLI_TOKEN, see wokwi.toml

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
