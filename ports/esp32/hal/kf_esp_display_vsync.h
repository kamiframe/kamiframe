/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Beam-racing control and stats for esp_display.cpp's push_rect(): the
 * feature that reads the ILI9341's Get_scanline register (0x45) before a
 * write and, if the panel's own scan-out is about to cross the rectangle
 * being written, waits for it to pass first -- so a write never lands
 * underneath the scan reading the same rows, which is what the remaining
 * partial-rectangle flicker (a moving highlight, a label updating) turned
 * out to be. See push_rect()'s own comment in esp_display.cpp for the wait
 * logic itself; this header is just the runtime control surface KFDBG VSYNC
 * and KFDBG STATE need (kf_dbg_bridge.cpp), matching kf_esp_display_diag.h's
 * split of "the mechanism lives in esp_display.cpp, the header is what an
 * outside caller needs."
 *
 * ON KF_DBG_BRIDGE_ENABLE=0: everything below still declares (the KFDBG
 * VSYNC command and the vsync_* fields in KFDBG STATE only exist under that
 * flag anyway, so nothing would call these), but push_rect() never performs
 * a wait in that configuration -- it behaves exactly as it did before this
 * feature existed. That is not a software choice made for convenience, it
 * is a hardware fact about this board: reading Get_scanline needs the
 * ILI9341's SDO line wired to the SPI peripheral's MISO input, and
 * kf_display_init() only reserves that pin (GPIO6, shared with the
 * backlight -- see kf_esp_pins.h) when KF_DBG_BRIDGE_ENABLE reserves it for
 * the SCANLINE diagnostic. Without the bridge, MISO is never configured on
 * this bus at all (bus_config.miso_io_num stays -1), so there is no signal
 * for a read to receive regardless of what this header declares -- see
 * kf_display_init()'s own comment in esp_display.cpp for the electrical
 * reasoning (GPIO6 is not this bus's native IOMUX pin, so reserving it drops
 * the WHOLE bus to GPIO-matrix routing, not a free change to make
 * unconditionally just to let a shipping build wait on scanline reads too).
 * A shipping build (KF_DBG_BRIDGE_ENABLE=0) therefore keeps writing
 * immediately, every time -- the same behaviour this codebase already had
 * before KFDBG SCANLINE was added, not a regression this feature
 * introduces.
 */

#ifndef KF_ESP_DISPLAY_VSYNC_H
#define KF_ESP_DISPLAY_VSYNC_H

#include "kf_dbg_bridge.h" /* KF_DBG_BRIDGE_ENABLE */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if KF_DBG_BRIDGE_ENABLE

/* KFDBG VSYNC <0|1>: turns the wait on or off at runtime, so the same board
 * can be measured both ways without a reflash -- toggle it, watch KFDBG
 * STATE's fps/frame_us and vsync_* fields, toggle it back. Default true
 * (see g_vsync_enabled's own comment in esp_display.cpp for why defaulting
 * ON is the right call given what is and is not confirmed about this
 * panel). Takes effect on the very next push_rect() call; there is no
 * frame-boundary latching to get right here, since push_rect() reads this
 * flag itself rather than caching it per frame. */
void kf_esp_display_vsync_set_enabled(bool enabled);

/* The current setting, for KFDBG STATE to report -- see kf_dbg_bridge.cpp's
 * handle_state(). */
bool kf_esp_display_vsync_enabled(void);

/* Stats for the most recently completed one-second window, for KFDBG STATE:
 *
 *   rects_written  every push_rect() call in that window, whether or not
 *                  vsync is enabled -- a baseline write-rate figure so
 *                  toggling vsync on and off can be compared against the
 *                  same workload rather than two different ones.
 *   rects_waited   how many of those actually blocked waiting for the scan
 *                  to clear the rectangle first. 0 whenever vsync is
 *                  disabled, or whenever every write this window found the
 *                  scan already past (the common case -- see push_rect()'s
 *                  comment on why most writes should cost one read and no
 *                  wait at all).
 *   avg_wait_us    the mean wait, in microseconds, across only the writes
 *                  that waited (rects_waited of them) -- 0 if rects_waited
 *                  is 0, not a divide-by-zero.
 *
 * "Most recently completed" rather than "the last full wall-clock second":
 * during a genuinely idle pet (the whole point of dirty-rect present --
 * zero bytes for a still screen) push_rect() may not run at all for
 * several seconds, and there is no background task ticking these counters
 * down to zero on a clock of their own. STATE keeps reporting the last
 * window that actually had writes in it rather than decaying to a
 * misleading 0/0/0 the moment the pet holds still -- a deliberate choice,
 * not an oversight; see kf_vsync_note_rect_written()'s own comment in
 * esp_display.cpp. All three are 0 before the very first push_rect() call
 * of the whole run. */
void kf_esp_display_vsync_get_stats(uint32_t *rects_written,
                                     uint32_t *rects_waited,
                                     uint32_t *avg_wait_us);

#endif /* KF_DBG_BRIDGE_ENABLE */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_DISPLAY_VSYNC_H */
