/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Panel profiles: everything that differs between one 240x320 SPI display
 * module and another, in one table per panel, so that supporting a new one
 * means adding a table rather than editing a driver.
 *
 * ============================================================================
 *  WHY THIS EXISTS
 *
 *  Kamiframe is meant to be buildable with whatever 240x320 SPI panel someone
 *  can actually source. Two are supported today and more are expected, so the
 *  panel-specific parts are data here rather than code in esp_display.cpp.
 *
 *  The bring-up in ADR 0024 is what makes the shape of this obvious. Two
 *  modules that are both "240x320 SPI, eight wires, wired identically" still
 *  differ in every single field below, and one of those differences -- byte
 *  order -- costs an evening if you meet it without knowing it exists.
 * ============================================================================
 *
 * WHAT VARIES BETWEEN PANELS, and why each field is here:
 *
 *   init table       Controllers do not share a command set. Worse, the same
 *                    controller on different glass needs different power and
 *                    gamma values, so this is per-MODULE, not per-controller.
 *
 *   use_builtin_init ESP-IDF's own ST7789 init sends four commands (SLPOUT,
 *                    MADCTL, COLMOD, RAMCTRL). That is enough for a
 *                    controller to accept traffic -- which is why every call
 *                    returns ESP_OK on a panel that shows nothing -- but not
 *                    enough to drive glass whose power-on defaults do not
 *                    suit it. The ILI9341 must skip it entirely: RAMCTRL
 *                    (0xB0) is a different register on that controller, so
 *                    sending it writes a wrong value to something real.
 *
 *   big_endian_fb    THE ONE THAT BITES. RGB565 goes on the wire high byte
 *                    first; the ESP32-S3 is little-endian; and esp_lcd byte-
 *                    reverses commands and parameters but NOT colour data.
 *                    So a uint16_t framebuffer transmits backwards. The
 *                    ST7789 hides this because RAMCTRL carries a
 *                    little-endian bit that esp_lcd sets from data_endian --
 *                    but that bit is only ever sent by esp_lcd_panel_init(),
 *                    which the ILI9341 path must skip, and the ILI9341 has no
 *                    equivalent register at all. It is always big-endian.
 *
 *                    Measured on real hardware (ADR 0024), full-screen fills:
 *                    0xF800 red showed blue, 0x07E0 green showed pink, 0x001F
 *                    blue showed green, while white and black were correct.
 *                    That is a plain byte swap and nothing else -- white and
 *                    black are invariant under both a byte swap and a BGR
 *                    fault, which is exactly what separates the two.
 *
 *   invert           ST7789 silicon defaults to inversion off, but many IPS
 *                    modules built around it are wired to need INVON. Per
 *                    module, not per controller.
 *
 *   x_gap / y_gap    Many 240x320 modules address a window offset inside a
 *                    larger controller frame buffer. Wrong offset means a
 *                    picture shifted by a few pixels with a stripe of
 *                    garbage down one edge.
 *
 * ADDING A PANEL: add a profile below, point KF_PANEL_PROFILE at it, and run
 * ports/esp32-bringup with its own panel selector set to match. Nothing in
 * esp_display.cpp should need to change. If it does, that is a missing field
 * here and the fix belongs in this file.
 */

#ifndef KF_PANEL_PROFILE_H
#define KF_PANEL_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One controller command: opcode, up to 16 parameter bytes, and a delay to
 * observe afterwards. 16 is sized for the longest real entry -- the ILI9341's
 * 15-byte gamma tables -- with one spare. */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} kf_panel_cmd;

typedef struct {
    const char *name;
    const kf_panel_cmd *init;
    size_t init_count;
    bool use_builtin_init;
    bool big_endian_fb;
    bool invert;
    int x_gap;
    int y_gap;
} kf_panel_profile;

/* -------------------------------------------------------------------------
 * ILI9341 -- HiLetgo 2.8in, module HSD028309.
 *
 * VERIFIED ON HARDWARE 2026-08-08 (ADR 0024). Confirmed by photographing the
 * bring-up test card: bars reaching all four edges (so no gap needed), the
 * green stripe on the left, and all eight colour patches correct.
 *
 * This table is copied from the bring-up diagnostic's own, which is the
 * version that actually produced a correct picture. Keep the two in step: if
 * one changes, the other is now lying about what was proven.
 * ------------------------------------------------------------------------- */
static const kf_panel_cmd kf_panel_ili9341_init[] = {
    {0xEF, {0x03, 0x80, 0x02}, 3, 0},          /* undocumented, in every driver */
    {0xCF, {0x00, 0xC1, 0x30}, 3, 0},          /* power control B */
    {0xED, {0x64, 0x03, 0x12, 0x81}, 4, 0},    /* power on sequence */
    {0xE8, {0x85, 0x00, 0x78}, 3, 0},          /* driver timing A */
    {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0}, /* power control A */
    {0xF7, {0x20}, 1, 0},                      /* pump ratio */
    {0xEA, {0x00, 0x00}, 2, 0},                /* driver timing B */
    {0xC0, {0x23}, 1, 0},                      /* PWCTR1  power control 1 */
    {0xC1, {0x10}, 1, 0},                      /* PWCTR2  power control 2 */
    {0xC5, {0x3E, 0x28}, 2, 0},                /* VMCTR1  vcom 1 */
    {0xC7, {0x86}, 1, 0},                      /* VMCTR2  vcom 2 */
    /* 0x88 = MY set, MX clear, BGR set: portrait with the module's pin header
     * at the TOP, which is the mounting this project settled on. Every Arduino
     * library prints 0x48 for this panel, which is the same portrait rotated
     * 180 degrees (header at the bottom). Only the two mirror bits differ, so
     * flipping the mounting later is one byte. */
    {0x36, {0x88}, 1, 0},                      /* MADCTL  orientation + BGR */
    {0x3A, {0x55}, 1, 0},                      /* COLMOD  16 bit */
    {0xB1, {0x00, 0x18}, 2, 0},                /* FRMCTR1 frame rate */
    {0xB6, {0x08, 0x82, 0x27}, 3, 0},          /* DFUNCTR display function */
    {0xF2, {0x00}, 1, 0},                      /* gamma disable */
    {0x26, {0x01}, 1, 0},                      /* gamma curve select */
    {0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
            0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15, 0}, /* GMCTRP1 */
    {0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
            0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15, 0}, /* GMCTRN1 */
    /* SLPOUT. The datasheet wants 120ms afterwards; 150 for margin. */
    {0x11, {0}, 0, 150},                       /* SLPOUT  wake up */
};

static const kf_panel_profile kf_panel_ili9341 = {
    "ILI9341 (HiLetgo 2.8in)",
    kf_panel_ili9341_init,
    sizeof(kf_panel_ili9341_init) / sizeof(kf_panel_ili9341_init[0]),
    /* use_builtin_init: false. esp_lcd_panel_init() would send RAMCTRL
     * (0xB0), which is a different register on this controller. */
    false,
    /* big_endian_fb: true. Measured -- see this file's header comment. */
    true,
    /* invert: false. The test card came out on a white field with inversion
     * off and a black one with it on. */
    false,
    0,
    0,
};

/* -------------------------------------------------------------------------
 * ST7789 -- Waveshare 2in, the PRIMARY panel for this project.
 *
 * NOT YET VERIFIED ON HARDWARE. The first unit was faulty -- its DC line
 * measured 0.7mV at the panel while every other line swung a clean 3.3V, and
 * soldering directly to the pad did not revive it (ADR 0024) -- and was
 * returned. A replacement has not arrived.
 *
 * So this profile is reasoned, not proven: the init values are the Waveshare
 * module's own sequence from the upstream Linux DRM driver written for this
 * exact board, and the flags follow from the controller's documented
 * behaviour. Expect to correct something here on the first real run. VCOMS,
 * VRHS, VDVS and PWCTRL1 are the four that decide whether anything is visible
 * at all; the gamma tables only change how colours look.
 * ------------------------------------------------------------------------- */
static const kf_panel_cmd kf_panel_st7789_init[] = {
    {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0}, /* PORCTRL  porch control */
    {0xB7, {0x35}, 1, 0},                         /* GCTRL    gate control */
    {0xBB, {0x1F}, 1, 0},                         /* VCOMS    common voltage */
    {0xC0, {0x2C}, 1, 0},                         /* LCMCTRL  lcd control */
    {0xC2, {0x01}, 1, 0},                         /* VDVVRHEN vdv/vrh enable */
    {0xC3, {0x12}, 1, 0},                         /* VRHS     vrh set */
    {0xC4, {0x20}, 1, 0},                         /* VDVS     vdv set */
    {0xC6, {0x0F}, 1, 0},                         /* FRCTRL2  frame rate */
    {0xD0, {0xA4, 0xA1}, 2, 0},                   /* PWCTRL1  power control */
    {0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
            0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23}, 14, 0}, /* PVGAMCTRL */
    {0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
            0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23}, 14, 0}, /* NVGAMCTRL */
};

static const kf_panel_profile kf_panel_st7789 = {
    "ST7789 (Waveshare 2in)",
    kf_panel_st7789_init,
    sizeof(kf_panel_st7789_init) / sizeof(kf_panel_st7789_init[0]),
    /* use_builtin_init: true. esp_lcd's ST7789 init handles SLPOUT, MADCTL,
     * COLMOD and RAMCTRL correctly for this controller; the table above only
     * adds the power and gamma registers it leaves at their defaults. */
    true,
    /* big_endian_fb: false. esp_lcd_panel_init() programs RAMCTRL's
     * little-endian bit from data_endian, so the framebuffer can go to the
     * panel with no host-side swapping at all. */
    false,
    /* invert: false, per the controller default. Worth suspecting first if
     * the replacement module comes up as a photographic negative -- many IPS
     * modules built on this controller are wired to need INVON. */
    false,
    0,
    0,
};

/* -------------------------------------------------------------------------
 * WHICH PANEL THIS BUILD DRIVES.
 *
 * Defaulted to the ILI9341 because that is the panel that physically exists
 * and is verified. A default that produces a black screen on the only board
 * in the world running this firmware would be a bad default, however
 * defensible on paper.
 *
 * REVISIT WHEN THE REPLACEMENT ST7789 ARRIVES: that is the primary panel, and
 * once it is verified this default should flip to it, with the ILI9341
 * remaining a supported option.
 *
 * Override without editing this file by defining KF_PANEL_PROFILE at build
 * time, e.g. -DKF_PANEL_PROFILE=kf_panel_st7789.
 * ------------------------------------------------------------------------- */
#ifndef KF_PANEL_PROFILE
#define KF_PANEL_PROFILE kf_panel_ili9341
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_PANEL_PROFILE_H */
