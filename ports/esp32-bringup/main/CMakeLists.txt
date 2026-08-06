## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## INCLUDE_DIRS reaches into the firmware's HAL directory for one header:
## kf_esp_pins.h. That is the whole point -- the diagnostic and the firmware
## read the same pin definitions, so a bring-up that passes cannot be
## testing different pins than the firmware will use.

idf_component_register(
    SRCS "bringup_main.cpp"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../../esp32/hal"
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_driver_sdspi
        esp_lcd
        fatfs
        sdmmc
        esp_timer
        esp_hw_support
        spi_flash
)
