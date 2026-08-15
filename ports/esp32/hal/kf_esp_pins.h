/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP32 GPIO pin assignments for every HAL backend in this directory, in one
 * place, on purpose: wiring a real board means reading exactly one file, and
 * changing a pin because the real layout wants something else means editing
 * exactly one file.
 *
 * ============================================================================
 *  THIS FILE IS THE ESP32-S3-DevKitC-1's PINOUT, and it is deliberately not
 *  named that yet.
 *
 *  A ProS3 (Unexpected Maker) was ordered 2026-08-14 and has not arrived. It
 *  is the same silicon with a completely different header, so it will need
 *  its own table -- and the DevKitC boards stay in service for software
 *  testing, so BOTH have to work. That makes this a build-time profile
 *  problem, the same shape kf_panel_profile.h already solves for panels:
 *  expect KF_BOARD with `devkitc` and `pros3` values when it lands.
 *
 *  DO NOT overwrite the numbers below with a ProS3's. They are measured
 *  (see the banner further down), they are the only pinout any hardware has
 *  ever been proven against, and losing them costs a bring-up. Add a second
 *  table; do not edit this one.
 *
 *  docs/hardware-bringup.md's "The ProS3 board" section has the full list of
 *  what changes -- including the microSD card gaining the ability to share
 *  the display's SPI bus (its new breakout has no buffer chip), and the
 *  buttons moving to an MCP23017 over I2C.
 * ============================================================================
 *
 * The human-readable version of this file, with wiring diagrams and an
 * assembly order, is docs/hardware-bringup.md. If you change a pin here,
 * change it there too -- that doc is what someone has open while holding a
 * soldering iron, and a doc that disagrees with the firmware is worse than
 * no doc.
 *
 * ============================================================================
 *  MEASURED, 2026-08-07/08. This file carried an "ASSUMPTION, NOT MEASURED"
 *  banner for all of Phase 1 and no longer needs one: every pin below has
 *  been wired to a real ESP32-S3-DevKitC-1 N16R8 and exercised by
 *  ports/esp32-bringup, which passed every stage -- backlight, panel over
 *  SPI, I2C with a DS3231 answering and keeping time across a power cut,
 *  a microSD card mounting and round-tripping a file, and all seven buttons.
 *
 *  One pin changed as a result: LCD_DC moved from GPIO9 to GPIO7. See its
 *  own comment below.
 *
 *  Still unmeasured, and called out where they live rather than here: the
 *  I2S lines at the bottom of this file (reserved, never wired), and
 *  KF_DISPLAY_TRANSFER_OVERHEAD_BYTES in kf/budget.h. KF_DISPLAY_SPI_HZ is
 *  no longer among them -- the bring-up clock sweep measured it at 40MHz.
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
 * Display: ST7789 or ILI9341 over SPI (SPI2_HOST).
 *
 * "No MISO -- this panel is write-only" lived here through Phase 1b and was
 * true of exactly one of the two panels this project supports. The
 * Waveshare ST7789 genuinely has no data-out pin. The HiLetgo ILI9341 that
 * is actually on the bench (see below) is not write-only at all -- it has a
 * real SDO pin, unused until KFDBG SCANLINE (2026-08-08) needed it to
 * investigate whether beam-racing is possible on this panel, at which point
 * it got wired to GPIO6. See KF_ESP_PIN_LCD_MISO below for the pin itself
 * and the collision it creates, and esp_display.cpp's kf_display_init() for
 * why the bus only reserves it when the active panel profile says it has a
 * read line to reserve it for (kf_panel_profile.h's has_read_line, ADR
 * 0039) AND KF_DBG_BRIDGE_ENABLE is on. Only the ILI9341 profile has
 * has_read_line == true, so in practice this reservation, and the collision
 * it creates, is an ILI9341-only concern -- the ST7789 profile never claims
 * this pin as anything but the backlight.
 *
 * These wires are the same for every 240x320 SPI module this project
 * supports -- what differs between controllers is the init sequence,
 * orientation, colour inversion and framebuffer byte order, none of which is
 * a pin. All of that lives in kf_panel_profile.h, one table per panel, and
 * esp_display.cpp reads it rather than hardcoding a controller.
 *
 * The panel working on the bench as of 2026-08-08 is an ILI9341 (HiLetgo
 * 2.8in, module HSD028309), and it is what this build drives by default. Its
 * settled configuration, measured rather than assumed, is recorded in ADR
 * 0024 and encoded in kf_panel_profile.h: MADCTL 0x88 with the pin header
 * mounted at the TOP, COLMOD 0x55, inversion OFF, and -- the one that costs
 * an evening if you miss it -- a BIG-ENDIAN framebuffer.
 *
 * The 2in ST7789 is the primary panel and is now hardware-verified too
 * (2026-08-13, ADR 0059) -- it is the default profile as of that date. It
 * needs one wiring change from the ILI9341 and no others: GPIO6 goes to its
 * BL pin rather than to a data-out line, because that module's eight-pin
 * flex has no SDO. Only one panel at a time, though -- they share CS.
 *
 * An earlier version of this comment said the ST7789 had never been
 * verified because the first unit was faulty. That diagnosis is doubtful;
 * see kf_panel_profile.h's ST7789 section and the LCD_DC comment just below,
 * which are the same story from two directions.
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

/* SDO(MISO), wired 2026-08-08 for the KFDBG SCANLINE diagnostic (see
 * ports/esp32/main/kf_dbg_bridge.cpp and esp_display.cpp's kf_display_init())
 * -- confirmed against the manufacturer's schematic that this is the
 * ILI9341 module's only data-out line; there is no separate tearing-effect
 * (TE) pin on this board's 18-pin flex to wire instead.
 *
 * KF_ESP_PIN_LCD_MISO IS KF_ESP_PIN_LCD_BL, above. Same GPIO, deliberately
 * defined twice with different names, because on THIS board's ILI9341
 * module there is nowhere else to put it -- see this file's own "19 of 23"
 * accounting above; a custom PCB does not have to repeat this trade.
 *
 * ADR 0039 resolved what used to be a real, unresolved collision: whether
 * GPIO6 means "read line" or "backlight" is now a property of the active
 * panel profile (kf_panel_profile.h's has_read_line), not a coincidence of
 * which two build flags happened to be set. esp_display.cpp's
 * kf_display_init() reserves the pin as MISO only when has_read_line is
 * true AND KF_DBG_BRIDGE_ENABLE is on, and configures it as the backlight
 * GPIO in every other case -- so the two roles are now mutually exclusive
 * by construction, not by one path silently declining to configure a pin
 * the other might be using.
 *
 * The two panels land on opposite sides of that split. On the ILI9341
 * (has_read_line == true) the pin becomes MISO whenever the bridge is
 * compiled in, and the collision this comment used to warn about is real on
 * paper but moot in practice: this module's own LED pin is wired straight
 * to 3V3, so nothing this board does through GPIO6 has ever controlled its
 * backlight regardless of which role the pin is playing. On the ST7789
 * (has_read_line == false, its module has no SDO pin at all) GPIO6 is
 * always the backlight, unconditionally, on every KF_DBG_BRIDGE_ENABLE
 * setting -- and that module's BL pin is real, so this is the only path in
 * the tree that will ever drive it. See esp_display.cpp's kf_display_init()
 * for the actual reservation logic and kf_display_set_backlight() for the
 * one caller that turns the pin on. */
#define KF_ESP_PIN_LCD_MISO GPIO_NUM_6

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
