/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP32 GPIO pin assignments for every HAL backend in this directory, in one
 * place, on purpose: wiring a real board means reading exactly one file, and
 * changing a pin because the real layout wants something else means editing
 * exactly one file.
 *
 * The human-readable version of this file, with wiring diagrams and an
 * assembly order, is docs/hardware-bringup.md. If you change a pin here,
 * change it there too -- that doc is what someone has open while holding a
 * soldering iron, and a doc that disagrees with the firmware is worse than
 * no doc.
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
 * HARD CONSTRAINTS, not guesses. Every assignment below avoids all of these:
 *
 *   GPIO26-32   the module's own quad SPI flash. Not broken out on the
 *               DevKitC-1 headers at all, so they are impossible to wire by
 *               accident -- listed for completeness.
 *   GPIO33-37   the octal PSRAM lines the N16R8 variant adds. THIS IS THE
 *               TRAP: 35, 36 and 37 *are* physically present on the J3
 *               header, right next to pins that are fine, and nothing stops
 *               you plugging a wire into one. Doing so does not fail loudly;
 *               it produces a board that boot loops or corrupts memory.
 *   GPIO19, 20  native USB (D-/D+).
 *   GPIO43, 44  UART0 -- the serial console every diagnostic here prints to.
 *   GPIO0, 3    strapping pins, sampled at reset.
 *   GPIO45, 46  strapping pins, sampled at reset.
 *   GPIO38/48   the onboard addressable RGB LED. Which one depends on board
 *               revision (v1.0 uses 48, v1.1 uses 38), so both are avoided
 *               rather than making the pinout depend on which board came out
 *               of the bag.
 *
 * That leaves 23 usable pins on the headers. 19 of them are assigned below,
 * which is worth knowing before designing a custom board: the devkit is
 * nearly full, and the flagship PCB will likely want an I2C GPIO expander
 * for the buttons rather than one pin each.
 */

#ifndef KF_ESP_PINS_H
#define KF_ESP_PINS_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Display: ST7789 over SPI (SPI2_HOST). No MISO -- this panel is write-only,
 * and leaving MISO unset (-1) tells the SPI driver not to reserve a pin for
 * it. See esp_display.cpp.
 *
 * The panel this is written against is a 240x320 ST7789. An ILI9341 of the
 * same resolution is NOT a drop-in: same wires, different init sequence, so
 * it needs esp_lcd_new_panel_ili9341() instead. Check which controller the
 * module actually has before wiring.
 *
 * The panel actually working on the bench as of 2026-08-08 is an ILI9341
 * (HiLetgo 2.8in, module HSD028309). Its settled configuration, measured
 * rather than assumed, is in ADR 0023: MADCTL 0x88 with the pin header
 * mounted at the TOP, COLMOD 0x55, inversion OFF, and -- the one that
 * costs an evening if you miss it -- a BIG-ENDIAN framebuffer, because the
 * ILI9341 has no equivalent of the ST7789's RAMCTRL little-endian bit and
 * esp_lcd does not byte-reverse colour data. esp_display.cpp still targets
 * the ST7789 and will need all of that if the ILI9341 becomes the panel.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_LCD_MOSI GPIO_NUM_11
#define KF_ESP_PIN_LCD_SCLK GPIO_NUM_12
#define KF_ESP_PIN_LCD_CS   GPIO_NUM_10
/* DC moved off GPIO9 to GPIO7 during first bring-up (2026-08-07). Two
 * reasons, one measured and one on principle.
 *
 * Measured: on the board in hand, GPIO9 driven high as a plain GPIO
 * produced under a millivolt at the panel, while RST, CS, CLK and DIN all
 * swung a clean 3.3V on the same breadboard through the same kind of
 * jumper. A new wire and a different cable changed nothing.
 *
 * On principle: GPIO9 is FSPIHD, one of SPI2's own IOMUX function pins on
 * this chip, and this build drives SPI2 on 10/11/12 which are the other
 * three. Borrowing the fourth as a hand-driven GPIO is the kind of thing
 * that works right up until it does not. GPIO7 has no such second job. */
#define KF_ESP_PIN_LCD_DC   GPIO_NUM_7
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

/* -------------------------------------------------------------------------
 * I2C bus, shared by every I2C peripheral on the board. The DS3231 RTC is
 * the only one that matters for bring-up -- it is what closes the
 * wall-clock-does-not-survive-power-off gap esp_time.cpp documents -- but
 * the IMU, ambient light sensor and haptic driver all live on these same two
 * wires when they get added, which is the entire point of I2C and the reason
 * only two pins are spent here.
 *
 * Internal pull-ups are enabled in software, but every breakout module in
 * this build (DS3231, BH1750, MPU-6050, DRV2605L) carries its own external
 * pull-up resistors, which are stronger and are what actually holds the bus.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_I2C_SDA GPIO_NUM_13
#define KF_ESP_PIN_I2C_SCL GPIO_NUM_14

/* Known I2C addresses on this bus, so a scan result can be read without
 * looking anything up. DS3231 and MPU-6050 both answer on 0x68 and cannot
 * share a bus without the MPU's AD0 pin pulled high to move it to 0x69. */
#define KF_ESP_I2C_ADDR_DS3231  0x68 /* RTC */
#define KF_ESP_I2C_ADDR_AT24C32 0x57 /* EEPROM on the same DS3231 module */
#define KF_ESP_I2C_ADDR_BH1750  0x23 /* ambient light */
#define KF_ESP_I2C_ADDR_DRV2605 0x5A /* haptic driver */
#define KF_ESP_I2C_ADDR_BME280  0x76 /* or 0x77 depending on SDO */
#define KF_ESP_I2C_ADDR_MPU6050 0x68 /* collides with DS3231 -- see above */

/* -------------------------------------------------------------------------
 * microSD card, on SPI3_HOST -- deliberately its OWN SPI bus, not shared
 * with the display on SPI2.
 *
 * Sharing one bus is the textbook arrangement and it would save four pins,
 * but the common level-shifted breakout modules (the 6-pin ones with a
 * regulator and a buffer on board) are well known for not releasing MISO
 * when their chip-select is high. On a shared bus that corrupts the
 * display's traffic intermittently, which presents as a display that mostly
 * works, and is a genuinely horrible first-hardware debugging session. Four
 * pins is a cheap price to make that class of bug impossible.
 *
 * A custom PCB with a card socket wired directly (no buffer chip) can share
 * the display bus safely. This constraint is about the breakout module, not
 * about SPI.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_SD_SCLK GPIO_NUM_39
#define KF_ESP_PIN_SD_MOSI GPIO_NUM_40
#define KF_ESP_PIN_SD_MISO GPIO_NUM_41
#define KF_ESP_PIN_SD_CS   GPIO_NUM_42

/* -------------------------------------------------------------------------
 * Not assigned yet, but reserved here so the four remaining free pins are
 * spoken for on paper before someone spends them on something else.
 *
 * The I2S output (MAX98357A amplifier) and I2S input (INMP441 microphone)
 * share BCLK and WS, which is the only reason both fit: a second, separate
 * I2S bus would need three more pins that do not exist. The passive buzzer
 * is then redundant with the amplifier and is not given a pin at all.
 * ------------------------------------------------------------------------- */
#define KF_ESP_PIN_I2S_BCLK  GPIO_NUM_1 /* shared: amp + mic */
#define KF_ESP_PIN_I2S_WS    GPIO_NUM_2 /* shared: amp + mic */
#define KF_ESP_PIN_I2S_DOUT  GPIO_NUM_9 /* to MAX98357A DIN; was 7, swapped with LCD_DC */
#define KF_ESP_PIN_I2S_DIN   GPIO_NUM_47 /* from INMP441 SD */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_PINS_H */
