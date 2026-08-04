## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## NOT BUILT YET. See ../README.md.
##
## ESP-IDF requires a component named `main`. When Phase 1b starts, this is
## where the ESP32 HAL backends and the FreeRTOS task that drives
## kf_app_frame() will live. It registers no sources today because writing a
## firmware entry point that has never been compiled produces code that has
## never been correct.

idf_component_register(
    SRCS
        # app_main.cpp          <- spawns the FreeRTOS task that calls kf_app_frame()
        # hal/esp_display.cpp   <- ST7789 over SPI + DMA
        # hal/esp_input.cpp     <- GPIO buttons, raw levels only
        # hal/esp_time.cpp      <- esp_timer for mono, external RTC for wall
        # hal/esp_memory.cpp    <- heap_caps_malloc, INTERNAL|DMA vs SPIRAM
        # hal/esp_entropy.cpp   <- esp_random
        # hal/esp_log.cpp       <- ESP_LOGx, and a panic that paints the panel
    INCLUDE_DIRS "."
    REQUIRES hakoniwaos
)
