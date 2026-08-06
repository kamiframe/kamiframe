## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## Hardware bring-up diagnostic -- a SEPARATE ESP-IDF project from
## ports/esp32, on purpose. It shares exactly one file with the firmware
## (../esp32/hal/kf_esp_pins.h) and depends on nothing else in this repo, so
## a failure here is always a wiring failure and never a hakoniwaos build
## problem. See main/bringup_main.cpp's header for the full reasoning.
##
##     cd ports/esp32-bringup
##     idf.py set-target esp32s3
##     idf.py flash monitor
##
## Deliberately no EXTRA_COMPONENT_DIRS: hakoniwaos is not built here.

cmake_minimum_required(VERSION 3.20)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(kamiframe-bringup)
