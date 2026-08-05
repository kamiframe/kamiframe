## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## ESP-IDF requires a component named `main`. This is deliberately a plain
## hello-world (ADR 0019), not a call into kf_app_frame() -- the ESP32 HAL
## backends that would need are Phase 1b's job, not this slice's. See
## main.c's own header comment for the full reasoning.
##
## REQUIRES hakoniwaos so this build also compiles every file in
## hakoniwaos/sources.cmake for the real target, even though main.c never
## calls into it -- see ../README.md for why that's the part worth proving
## first.

idf_component_register(
    SRCS
        main.c
        # app_main.cpp          <- spawns the FreeRTOS task that calls kf_app_frame(), Phase 1b
        # hal/esp_display.cpp   <- ST7789 over SPI + DMA
        # hal/esp_input.cpp     <- GPIO buttons, raw levels only
        # hal/esp_time.cpp      <- esp_timer for mono, external RTC for wall
        # hal/esp_memory.cpp    <- heap_caps_malloc, INTERNAL|DMA vs SPIRAM
        # hal/esp_entropy.cpp   <- esp_random
        # hal/esp_log.cpp       <- ESP_LOGx, and a panic that paints the panel
    INCLUDE_DIRS "."
    REQUIRES hakoniwaos spi_flash
)
