/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * One raw SPI primitive, for one caller: kf_dbg_bridge.cpp's KFDBG SCANLINE
 * handler, which exists to answer a single question -- is beam-racing
 * (delaying a write until the panel's own scan-out has passed the rectangle
 * about to be touched) even possible on this module. The panel's 18-pin flex
 * carries no tearing-effect (TE) pin, confirmed against the manufacturer's
 * schematic, so the only remaining way to find out is polling the ILI9341's
 * Get_Scanline register (command 0x45) and seeing whether the replies make
 * sense. This header is the plumbing that makes one such read possible; it
 * does not interpret the result or decide anything -- see kf_dbg_bridge.cpp
 * for the sampling loop, the statistics, and the JSON it reports.
 *
 * Intentionally its OWN tiny header rather than new entries in
 * kf/hal/display.h (hakoniwaos/): that interface is the cross-platform HAL
 * contract every backend -- SDL, headless, this one -- implements
 * identically, and "read a controller register back over SPI" is not a
 * concept the other two backends have any way to honour. There is no SPI bus
 * to poll when the framebuffer is just a block of host memory. Keeping this
 * ESP32-specific and out of hakoniwaos/ draws the same boundary
 * kf_esp_pins.h and kf_panel_profile.h already draw for this port's own
 * concerns.
 *
 * esp_display.cpp owns g_io, the one esp_lcd_panel_io_handle_t this reads
 * through -- see that file's kf_display_init() for why a genuinely slower,
 * separate read clock was reasoned about and deliberately NOT built (the
 * CS-pin-sharing hazard documented there), and why this reads at the same
 * clock the display's own writes use instead.
 */

#ifndef KF_ESP_DISPLAY_DIAG_H
#define KF_ESP_DISPLAY_DIAG_H

#include "kf_dbg_bridge.h" /* KF_DBG_BRIDGE_ENABLE */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything below only exists when the debug bridge does -- its one
 * caller, kf_dbg_bridge.cpp's handle_scanline(), lives entirely inside that
 * same #if, so there is no configuration in which a declaration here would
 * go uncalled, and no reason to pay for a callable-but-pointless stub the
 * way kf_dbg_input_mask() does for a function every frame calls
 * unconditionally. This one isn't. */
#if KF_DBG_BRIDGE_ENABLE

/* ILI9341's Get_Scanline command. Defined in the MIPI DCS command set, not
 * an ILI-proprietary opcode, so it is very likely valid against the ST7789
 * profile too -- but only the ILI9341 currently on the bench is what this
 * diagnostic has actually been reasoned about against, per the task that
 * added it (ADR 0024's HiLetgo 2.8in module). Treat a SCANLINE run against
 * KF_PANEL_PROFILE=kf_panel_st7789 as doubly unverified. */
#define KF_ESP_DISPLAY_DIAG_CMD_GET_SCANLINE 0x45

/* Issues one Get_Scanline read: command 0x45, then `byte_count` bytes of
 * raw response into `out_bytes`. Returns true and fills out_bytes on
 * success; false (out_bytes left untouched) if the display isn't up yet or
 * the SPI transaction itself failed -- either way, safe to call at any time
 * this build has the debug bridge compiled in, and the caller is expected
 * to treat a false return as one failed sample, not a reason to stop the
 * run (see kf_dbg_bridge.cpp's handle_scanline()).
 *
 * The reply's byte layout is NOT interpreted here. The ILI9341's SPI read
 * protocol is widely documented (and NOT verified against this specific
 * board, for want of hardware) to prepend one dummy byte before the real
 * data on a parameter read -- kf_dbg_bridge.cpp's caller assumes byte_count
 * == 3 (1 dummy + 2 data) and slices accordingly; this function has no
 * opinion on the count and will happily read however many bytes it's
 * asked for. */
bool kf_esp_display_diag_read_scanline(uint8_t *out_bytes, size_t byte_count);

/* The SPI clock kf_esp_display_diag_read_scanline() actually reads at, in
 * Hz, so a diagnostic reporting it doesn't have to duplicate the decision.
 * Today this returns exactly KF_DISPLAY_SPI_HZ -- the same clock writes
 * use. See kf_display_init()'s MISO comment in esp_display.cpp for why a
 * separate, slower, named read clock was the plan going in and was NOT
 * built: it would need a second SPI device sharing this display's CS pin,
 * and ESP-IDF's driver reassigns a shared CS pin's GPIO-matrix routing to
 * whichever device was added last, breaking the survivor's own CS the
 * moment the temporary one is torn down -- confirmed by reading
 * spi_common.c, not assumed. */
uint32_t kf_esp_display_diag_read_hz(void);

#endif /* KF_DBG_BRIDGE_ENABLE */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_DISPLAY_DIAG_H */
