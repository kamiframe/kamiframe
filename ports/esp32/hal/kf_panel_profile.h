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
 *   has_read_line    Whether the module brings an SDO pin out to a header
 *                    pin this board has wired. Added in ADR 0039, alongside
 *                    the discovery that this field -- not KF_DBG_BRIDGE_
 *                    ENABLE -- is what decides whether GPIO6 on this board
 *                    is a read line (KFDBG SCANLINE/VSYNC) or the backlight.
 *                    See the field's own comment on kf_panel_profile below
 *                    for the two panels' actual header pinouts.
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

    /* has_read_line   True if the physical module exposes a data-out (SDO)
     *                  pin on a line this board actually wires -- what makes
     *                  esp_lcd_panel_io_rx_param() possible at all, since SPI
     *                  cannot read without a MISO signal in the bus config.
     *                  This is a property of the MODULE, not the controller:
     *                  the ILI9341 and ST7789 controllers both implement
     *                  Get_Scanline-style parameter reads in silicon, but
     *                  whether that silicon's SDO pin is brought out to a
     *                  header pin this board can reach is a per-module wiring
     *                  fact.
     *
     *                  true for the HiLetgo ILI9341: it has a real SDO,
     *                  wired to GPIO6 for KFDBG SCANLINE (ADR 0024).
     *
     *                  false for the Waveshare 2in ST7789: its eight-pin
     *                  header is VCC GND DIN CLK CS DC RST BL -- DIN is
     *                  MOSI-only, and there is no ninth pin for SDO. Nothing
     *                  on this board could read from that panel even if the
     *                  ST7789 controller itself supports it.
     *
     *                  esp_display.cpp's kf_display_init() reads this field
     *                  to decide the GPIO6 question: reserve it as MISO (a
     *                  read line, and therefore NOT the backlight) only when
     *                  this is true AND the debug bridge is compiled in.
     *                  false means the pin is free for the backlight
     *                  unconditionally -- see esp_display.cpp and
     *                  kf_esp_pins.h's KF_ESP_PIN_LCD_MISO/KF_ESP_PIN_LCD_BL
     *                  for the collision this field resolves, and ADR 0039
     *                  for why the decision lives here instead of on
     *                  KF_DBG_BRIDGE_ENABLE alone. kf_dbg_bridge.cpp's
     *                  SCANLINE and VSYNC handlers also read this (via
     *                  kf_esp_display_diag.h's kf_esp_display_has_read_line())
     *                  to refuse outright on a profile with nothing to read,
     *                  rather than reporting whatever a floating input
     *                  returns. */
    bool has_read_line;

    /* spi_hz     The clock this MODULE has been measured to render
     *            correctly at. Per-panel, and it has to be: the ILI9341
     *            came out SOLID WHITE at 80MHz (ADR 0024's own sweep) while
     *            the ST7789 renders the real game correctly there
     *            (2026-08-14). One global constant would mean either
     *            throttling the good panel to the bad one's limit, or
     *            handing the bad one a clock that produces a white screen
     *            with a completely clean log -- the exact failure mode this
     *            whole file exists to prevent.
     *
     *            MEASURED, both of them, by ports/esp32-bringup's stage 2b
     *            sweep. Do not raise either without re-running it: marginal
     *            SPI drops occasional bits rather than failing outright, so
     *            a value that "looks fine" for a minute is not evidence.
     *
     *            Treat these as floors, not ceilings. Both were measured
     *            through breadboard jumpers, which is the worst wiring this
     *            project will ever have; a real PCB should do at least as
     *            well. */
    uint32_t spi_hz;
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
static constexpr kf_panel_cmd kf_panel_ili9341_init[] = {
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

static constexpr kf_panel_profile kf_panel_ili9341 = {
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
    /* has_read_line: true. This HiLetgo module has a real SDO pin -- wired
     * to GPIO6 for KFDBG SCANLINE (ADR 0024) -- and its LED pin is soldered
     * straight to 3V3, so nothing this board does can ever turn its
     * backlight off in software regardless of GPIO6's role. */
    true,
    /* spi_hz: 40MHz. Measured 2026-08-08 -- 80MHz came out solid white on
     * this module, which is what wholesale data corruption looks like here.
     * About four times this panel's datasheet write-cycle figure already, so
     * it is outside spec and works anyway, in line with what practically
     * every driver for it does. */
    40000000u,
};

/* -------------------------------------------------------------------------
 * ST7789 -- Waveshare 2in, the PRIMARY panel for this project.
 *
 * VERIFIED ON HARDWARE 2026-08-13 (ADR 0059). The home screen rendered
 * correctly on a replacement module: geometry reaching all four edges with no
 * gap, and colour confirmed correct rather than coincidentally-correct. The
 * Home background at the time was a pale green-tinted white whose red/blue
 * swap is nearly indistinguishable by eye on near-white -- but the same swap
 * would turn the orange needs bars teal, and they were orange. Channel order
 * is right. (That background has since been changed to a warm cream on
 * Chris's call; the proof is unaffected, since it rests on the saturated
 * bars, not on the background. See KF_CREATURE_PRESENTER_BG.)
 *
 * The init values below are the Waveshare module's own sequence from the
 * upstream Linux DRM driver written for this exact board, and needed no
 * correction. Exactly one field did: `invert`. See its comment below.
 *
 * If a future module of this type shows nothing at all, VCOMS, VRHS, VDVS and
 * PWCTRL1 are the four that decide whether anything is visible; the gamma
 * tables only change how colours look.
 *
 * ON THE FIRST UNIT, WHICH WAS RETURNED AS FAULTY. That diagnosis is now
 * doubtful and should not be used as prior evidence against a panel. It was
 * condemned on a 0.7mV reading at its DC line -- in the same session, on the
 * same signal, that condemned GPIO9 for producing a 0.7mV reading at its DC
 * line. GPIO9 is FSPIHD, so if SPI2 had claimed it via IOMUX, DC would carry
 * hold-line traffic no matter how sound the wire was, which accounts for the
 * whole observation without a panel fault. This replacement lit up on the
 * second flash with DC on GPIO7. See ADR 0059; ADR 0024 keeps its original
 * account as the record of what was believed at the time.
 * ------------------------------------------------------------------------- */
static constexpr kf_panel_cmd kf_panel_st7789_init[] = {
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

static constexpr kf_panel_profile kf_panel_st7789 = {
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
    /* invert: TRUE. MEASURED 2026-08-13, and the one field this profile got
     * wrong. The first run that ever put a picture on this panel came up as a
     * photographic negative; setting this cleared it and nothing else needed
     * to change. It was false here on the reasoning that the ST7789 defaults
     * to inversion off -- true of the silicon, false of this module, because
     * many IPS panels built around this controller are wired to need INVON.
     * Per module, not per controller, which is the whole reason this is a
     * profile field and not a driver constant. */
    true,
    0,
    0,
    /* has_read_line: false. The Waveshare 2in module's eight-pin flex is
     * VCC GND DIN CLK CS DC RST BL -- DIN is MOSI-only, there is no ninth
     * pin for SDO, and no separate tearing-effect (TE) pin either. This
     * board cannot read from this panel at all, on any GPIO. That is what
     * makes BL a real, otherwise-unclaimed pin on this profile: GPIO6 is
     * never reserved as MISO for a signal that does not physically exist,
     * so kf_display_init() always configures it as the backlight output
     * here -- see esp_display.cpp and ADR 0039. */
    false,
    /* spi_hz: 80MHz. Measured 2026-08-14 -- the bring-up sweep held at 80,
     * and the real game then rendered correctly there, which is the
     * stronger evidence of the two (a seven-second test card cannot see an
     * occasional dropped bit; minutes of text on screen can).
     *
     * This is the ceiling of the road, not of the panel: 80MHz is about as
     * fast as the S3's SPI peripheral drives a display at all. Going faster
     * means a parallel interface and a panel built for one -- see
     * kf/budget.h's own table.
     *
     * Halves a full-screen frame from ~31ms to ~15ms against a 33ms budget,
     * which is the single biggest change to this device's animation
     * headroom that has been available. */
    80000000u,
};

/* -------------------------------------------------------------------------
 * WHICH PANEL THIS BUILD DRIVES.
 *
 * The ST7789, as of 2026-08-13 (ADR 0059). This is the flip the previous
 * version of this comment asked a future reader to make: the default was the
 * ILI9341 only because it was the sole panel that had ever displayed
 * anything, and a default producing a black screen on the only board in the
 * world running this firmware would have been a bad default however
 * defensible on paper. Both panels display correctly now, so the default goes
 * to the 2in ST7789 -- the primary panel for the product, per CLAUDE.md's
 * hardware target and docs/hardware-bringup.md's parts table.
 *
 * The ILI9341 remains fully supported, not deprecated: it is the panel the
 * KFDBG SCANLINE/VSYNC diagnostics need (it is the only profile with
 * has_read_line == true), and it is the 2.8in size some builders will prefer.
 *
 * THE FOOTGUN THIS CREATES, stated plainly because it has already cost a
 * session in the other direction: KF_PANEL is a CMake CACHE variable, so it
 * persists in a build directory once set. Building for the other panel means
 * passing it explicitly -- `idf.py -DKF_PANEL=ili9341 build` -- and flashing
 * the wrong profile gives a black screen or wrong colours with a completely
 * clean log, because every esp_lcd call returns ESP_OK against glass showing
 * nothing.
 *
 * Override without editing this file by defining KF_PANEL_PROFILE at build
 * time, e.g. -DKF_PANEL_PROFILE=kf_panel_ili9341.
 * ------------------------------------------------------------------------- */
#ifndef KF_PANEL_PROFILE
#define KF_PANEL_PROFILE kf_panel_st7789
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_PANEL_PROFILE_H */
