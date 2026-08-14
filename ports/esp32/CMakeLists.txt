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
##     idf.py build                       # KF_PANEL defaults to st7789
##     idf.py -DKF_PANEL=ili9341 build    # the 2.8in alternative -- ADR 0059
##     idf.py flash monitor               # needs real hardware
##     idf.py wokwi                       # needs a WOKWI_CLI_TOKEN, see wokwi.toml

cmake_minimum_required(VERSION 3.20)

## KF_HOME_SCREEN (Task 5 of the Lua game-layer plan, the Lua game-layer
## plan): which Home implementation this firmware wires up -- "lua"
## (examples/creature_demo/creature.lua, declaring the screen through
## kf/scene.h directly -- the default since Task 6, ADR 0043) or "cpp"
## (kf_creature_screen.cpp, kept as the parity reference and fallback for
## one release). Set here, at the TOP of the whole project file, rather
## than in main/CMakeLists.txt where KF_PANEL lives: both main and the
## kamiframe_lvgl_port component (kamiframe_lvgl_ port/CMakeLists.txt,
## added to EXTRA_COMPONENT_DIRS below) need to see it, and a plain CACHE
## variable set here is visible to every component ESP-IDF's build goes on
## to configure, in either order.
##
##     idf.py -DKF_HOME_SCREEN=cpp build   # the fallback, for comparison
##
## Same CACHE STRING + configure-time FATAL_ERROR pattern as KF_PANEL
## below, for the identical reason: a typo should read as a command-line
## mistake, not a mysterious preprocessor failure three components away.
set(KF_HOME_SCREEN "lua" CACHE STRING "Home screen implementation: cpp or lua")
if(NOT KF_HOME_SCREEN STREQUAL "cpp" AND NOT KF_HOME_SCREEN STREQUAL "lua")
    message(FATAL_ERROR
        "KF_HOME_SCREEN must be 'cpp' or 'lua', got '${KF_HOME_SCREEN}'")
endif()

## KF_ENABLE_LVGL (ADR 0045, Task 2 of the screens/clock/sleep plan): OFF
## by default, same reasoning as the desktop build's identically-named
## option (top-level CMakeLists.txt). Info -- the screen that used to be
## this component's only real job -- moved to a kf.screen() group over the
## retained scene, so a default build no longer needs LVGL linked in at
## all. Set here, at the top, for the same reason KF_HOME_SCREEN is:
## EXTRA_COMPONENT_DIRS below decides, once and for all component-discovery
## passes, whether the kamiframe_lvgl_port DIRECTORY even exists as a
## candidate component -- ESP-IDF has no notion of an optional component
## once one is named, so leaving it off this list entirely (rather than
## registering it and hoping nothing links it) is what actually keeps LVGL
## out of a default build's image.
##
##     idf.py -DKF_ENABLE_LVGL=ON build   # the LVGL-vs-custom-engine
##                                          evaluation CLAUDE.md defers
set(KF_ENABLE_LVGL OFF CACHE BOOL "Build LVGL and its two LVGL-only tests")

## Also exported into the process ENVIRONMENT, not just the CACHE -- found
## the hard way (a real `idf.py build` REQUIRES-graph mismatch, not
## guessed): ../main/CMakeLists.txt's own conditional REQUIRES needs this
## value inside ESP-IDF's component_get_requirements.cmake scan, which runs
## as a genuinely SEPARATE `cmake -P` process ahead of the real configure
## and inherits none of this process's CMake variables, cache included --
## only whatever ESP-IDF explicitly threads through as generated build/
## component property files, which a plain custom CACHE variable is not
## one of. Environment variables DO cross that process boundary. See
## ../main/CMakeLists.txt's own comment on the read side for the full
## trace of how this was actually diagnosed.
set(ENV{KF_ENABLE_LVGL} "${KF_ENABLE_LVGL}")

# Point ESP-IDF at the shared module directories. hakoniwaos/CMakeLists.txt
# detects ESP_PLATFORM and registers itself as a component instead of an
# ordinary static library, reading the same sources.cmake the desktop build
# uses. kamiframe_lvgl_port (ADR 0027) is this port's own component -- it
# has no desktop-mode branch to speak of, so it lives under ports/esp32/
# itself rather than at the repo root the way hakoniwaos does, but it earns
# a real component the same way for the same reason: it pulls in a
# third-party dependency (LVGL) of its own, unlike a plain file like
# kf_pet_session.cpp (see ../main/CMakeLists.txt's comment on that). Listed
# only under KF_ENABLE_LVGL (ADR 0045) -- see that variable's own comment
# just above.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../hakoniwaos"
    "${CMAKE_CURRENT_LIST_DIR}/components/lua"
)
if(KF_ENABLE_LVGL)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/kamiframe_lvgl_port")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(kamiframe-firmware)
