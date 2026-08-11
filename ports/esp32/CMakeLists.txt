## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## ESP-IDF project entry point. ADR 0019's hello-world (Phase 1's second
## named hardware exit trigger, see README.md in this directory) is gone --
## Phase 1b built the real ESP32 HAL backends kf_app_frame() needs, and this
## now builds the whole firmware: app_main.cpp's real kf_app_init()/
## kf_app_frame() loop against those backends in ../hal/, not a stub. See
## ../main/CMakeLists.txt for what actually gets compiled and why.
##
##     cd ports/esp32
##     idf.py set-target esp32s3
##     idf.py build                       # KF_PANEL defaults to ili9341
##     idf.py -DKF_PANEL=st7789 build     # the primary panel -- ADR 0039
##     idf.py flash monitor               # needs real hardware
##     idf.py wokwi                       # needs a WOKWI_CLI_TOKEN, see wokwi.toml

cmake_minimum_required(VERSION 3.20)

## KF_HOME_SCREEN (Task 5 of the Lua game-layer plan, docs/superpowers/
## plans/2026-08-12-lua-game-layer.md): which Home implementation this
## firmware wires up -- "cpp" (kf_creature_screen.cpp, the default) or
## "lua" (examples/creature_demo/creature.lua, declaring the same screen
## through kf/scene.h directly). Set here, at the TOP of the whole project
## file, rather than in main/CMakeLists.txt where KF_PANEL lives: both
## main and the kamiframe_lvgl_port component (kamiframe_lvgl_port/
## CMakeLists.txt, added to EXTRA_COMPONENT_DIRS below) need to see it, and
## a plain CACHE variable set here is visible to every component ESP-IDF's
## build goes on to configure, in either order.
##
##     idf.py -DKF_HOME_SCREEN=lua build
##
## Same CACHE STRING + configure-time FATAL_ERROR pattern as KF_PANEL
## below, for the identical reason: a typo should read as a command-line
## mistake, not a mysterious preprocessor failure three components away.
set(KF_HOME_SCREEN "cpp" CACHE STRING "Home screen implementation: cpp or lua")
if(NOT KF_HOME_SCREEN STREQUAL "cpp" AND NOT KF_HOME_SCREEN STREQUAL "lua")
    message(FATAL_ERROR
        "KF_HOME_SCREEN must be 'cpp' or 'lua', got '${KF_HOME_SCREEN}'")
endif()

# Point ESP-IDF at the shared module directories. hakoniwaos/CMakeLists.txt
# detects ESP_PLATFORM and registers itself as a component instead of an
# ordinary static library, reading the same sources.cmake the desktop build
# uses. kamiframe_lvgl_port (ADR 0027) is this port's own component -- it
# has no desktop-mode branch to speak of, so it lives under ports/esp32/
# itself rather than at the repo root the way hakoniwaos does, but it earns
# a real component the same way for the same reason: it pulls in a
# third-party dependency (LVGL) of its own, unlike a plain file like
# kf_pet_session.cpp (see ../main/CMakeLists.txt's comment on that).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../hakoniwaos"
    "${CMAKE_CURRENT_LIST_DIR}/components/lua"
    "${CMAKE_CURRENT_LIST_DIR}/kamiframe_lvgl_port"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(kamiframe-firmware)
