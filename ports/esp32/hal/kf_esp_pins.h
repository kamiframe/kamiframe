/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP32 GPIO pin assignments for every HAL backend in this directory, in one
 * place, on purpose: wiring a real board means reading exactly one file, and
 * changing a pin because the real layout wants something else means editing
 * exactly one file.
 *
 * ============================================================================
 *  ASSUMPTION, NOT MEASURED. Same caveat kf/budget.h applies to
 *  KF_DISPLAY_SPI_HZ: nothing here has been wired to a real board yet (ADR
 *  0020). These are a reasonable, GPIO-matrix-friendly starting point for an
 *  ESP32-S3-WROOM-1 N16R8 devkit, not a verified pinout. Correct these at
 *  bring-up, the same as every other "ASSUMPTION, NOT MEASURED" figure in
 *  this codebase.
 * ============================================================================
 *
 * ONE HARD CONSTRAINT, not a guess: GPIO26-32 are wired inside the N16R8
 * module itself to the octal PSRAM and are not available for anything else.
 * Every assignment below deliberately avoids that range, along with the
 * strapping pins (0, 3, 45, 46) and the native-USB pins (19, 20) -- getting
 * any of those wrong does not fail loudly, it produces a board that boot
 * loops or enumerates strangely, which is a much worse debugging session
 * than a display that is wired to the wrong pin.
 */

#ifndef KF_ESP_PINS_H
#define KF_ESP_PINS_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Display: ST7789 over SPI. No MISO -- this panel is write-only, and
 * leaving MISO unset (-1) tells the SPI driver not to reserve a pin for it.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_LCD_MOSI GPIO_NUM_11
#define KF_ESP_PIN_LCD_SCLK GPIO_NUM_12
#define KF_ESP_PIN_LCD_CS   GPIO_NUM_10
#define KF_ESP_PIN_LCD_DC   GPIO_NUM_9
#define KF_ESP_PIN_LCD_RST  GPIO_NUM_8
#define KF_ESP_PIN_LCD_BL   GPIO_NUM_6

/* -------------------------------------------------------------------------
 * Buttons. Active-low: each button ties its GPIO to GND when pressed, and
 * every pin below is configured with its internal pull-up enabled, so a
 * button needs nothing but two wires -- no external resistor. See
 * esp_input.cpp.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_BTN_UP    GPIO_NUM_4
#define KF_ESP_PIN_BTN_DOWN  GPIO_NUM_5
#define KF_ESP_PIN_BTN_LEFT  GPIO_NUM_15
#define KF_ESP_PIN_BTN_RIGHT GPIO_NUM_16
#define KF_ESP_PIN_BTN_A     GPIO_NUM_17
#define KF_ESP_PIN_BTN_B     GPIO_NUM_18
#define KF_ESP_PIN_BTN_MENU  GPIO_NUM_21

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_PINS_H */
