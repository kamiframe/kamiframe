/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Raw SPI plumbing, for one caller: kf_dbg_bridge.cpp's KFDBG SCANLINE
 * handler, which exists to answer a single question -- is beam-racing
 * (delaying a write until the panel's own scan-out has passed the rectangle
 * about to be touched) even possible on this module. The panel's 18-pin flex
 * carries no tearing-effect (TE) pin, confirmed against the manufacturer's
 * schematic, so the only remaining way to find out is polling the ILI9341's
 * Get_Scanline register (command 0x45) and seeing whether the replies make
 * sense. This header is the plumbing that makes such a read possible, at
 * whatever clock the caller asks for; it does not interpret the result or
 * decide anything -- see kf_dbg_bridge.cpp for the sampling loop, the
 * statistics, and the JSON it reports.
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
 * through, and rebuild_panel_io(), the one function that can (re)create it.
 * A genuinely slower, separate read clock was reasoned about and rejected in
 * an earlier pass at this diagnostic -- adding a second SPI device sharing
 * this display's CS pin corrupts BOTH devices' CS routing on ESP-IDF's
 * driver; see kf_esp_display_diag_begin_probe()'s own comment in
 * esp_display.cpp for the confirmed mechanics. The fix that earlier pass
 * didn't build is the one below: tear the one IO handle down and rebuild it
 * at a slow clock for the duration of a probe, then rebuild it back.
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
 * Reads at whatever clock g_io is CURRENTLY built for in esp_display.cpp --
 * KF_DISPLAY_SPI_HZ (the normal write clock) outside of a SCANLINE probe,
 * or whatever kf_esp_display_diag_begin_probe() below most recently built
 * it at, in between that call and kf_esp_display_diag_end_probe().
 *
 * The reply's byte layout is NOT interpreted here. The ILI9341's SPI read
 * protocol is widely documented to prepend one dummy byte before the real
 * data on a parameter read; a clean 1/2/4MHz sweep against this specific
 * board has since found that documented framing WRONG for this module --
 * the real framing has no dummy byte at all (raw[0]/raw[1] is the 10-bit
 * value). kf_dbg_bridge.cpp's SCANLINE handler still reads byte_count == 3
 * and computes both framings from the same three bytes (now reporting the
 * confirmed one under its unprefixed JSON fields and the datasheet's under
 * the alt_-prefixed ones -- the reverse of this diagnostic's first cut, see
 * that file's own comment on the swap), so a human can keep comparing both
 * hypotheses on a future panel this has not been confirmed against; this
 * function itself has no opinion on the count and will happily read
 * however many bytes it's asked for -- esp_display.cpp's push_rect() (the
 * vsync feature this diagnostic justified) asks for only 2, since it only
 * ever needs the confirmed framing. */
bool kf_esp_display_diag_read_scanline(uint8_t *out_bytes, size_t byte_count);

/* Tears down the panel IO/panel esp_display.cpp normally drives writes
 * through and rebuilds both at read_hz, re-sending the panel's init table
 * so the controller is in a known state at the new clock before any reads
 * are attempted. Every kf_esp_display_diag_read_scanline() call after this
 * (until kf_esp_display_diag_end_probe() below) reads at read_hz instead of
 * the normal write clock -- see that function's own comment in
 * esp_display.cpp for the full mechanics, the CS-pin hazard this sidesteps,
 * and the thread-safety argument for why doing this mid-frame is safe.
 *
 * Returns false if the rebuild itself failed. The caller is expected to
 * proceed with its sampling loop regardless -- every read will simply fail,
 * which is itself a reportable result -- and to call
 * kf_esp_display_diag_end_probe() afterwards unconditionally either way. */
bool kf_esp_display_diag_begin_probe(uint32_t read_hz);

/* The other half of kf_esp_display_diag_begin_probe(): rebuilds the panel
 * IO/panel back at KF_DISPLAY_SPI_HZ, the normal write clock, and re-sends
 * the init table. Must be called exactly once after every
 * kf_esp_display_diag_begin_probe() call, even if that call returned false,
 * so a probe always attempts to leave the display usable again. If this
 * rebuild also fails, the display is left with no panel at all
 * (kf_display_present() already tolerates that -- see its own
 * g_panel == nullptr check -- so the failure mode is "no more screen
 * updates," not a crash); see this function's own comment in
 * esp_display.cpp for why that is the safe degradation to pick here. */
void kf_esp_display_diag_end_probe(void);

#endif /* KF_DBG_BRIDGE_ENABLE */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_DISPLAY_DIAG_H */
