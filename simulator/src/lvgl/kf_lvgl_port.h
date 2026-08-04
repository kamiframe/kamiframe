/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Ties the LVGL port glue together: lv_init(), the display bridge, the
 * keypad input bridge, and logging through kf_log. One call to bring LVGL
 * up, one call per frame to pump it, one call to tear it down.
 *
 * This is simulator-only, on purpose -- see ADR 0013's "what this slice
 * actually builds": LVGL is not wired into hakoniwaos/ (core) in this
 * slice, only into the desktop and headless backends, so nothing here
 * claims ESP32-readiness it has not earned.
 */

#ifndef KF_LVGL_PORT_H
#define KF_LVGL_PORT_H

#include <lvgl.h>

/* lv_init(), the display driver, the keypad input driver, and the log
 * bridge, in that order. Call once, after kf_fb_init() (the display bridge
 * writes into the existing framebuffer) and after kf_arena_init_all() (its
 * memory pool comes from KF_ARENA_LVGL). Returns the group widgets should
 * join to be reachable with the buttons -- see kf_lvgl_input.h. */
lv_group_t *kf_lvgl_port_init(void);

/* Advance LVGL's clock and let it run any pending timers/animations/redraw,
 * then flush. Call once per frame.
 *
 * `synthetic_frame_delta_ms`: 0 means "use real elapsed time"
 * (kf_lvgl_tick_advance_real(), the SDL case). Non-zero means "advance by
 * exactly this many milliseconds, not real time"
 * (kf_lvgl_tick_advance_synthetic(), the headless/determinism case). See
 * kf_lvgl_tick.h for why the two must not be conflated. */
void kf_lvgl_port_pump(uint32_t synthetic_frame_delta_ms);

void kf_lvgl_port_shutdown(void);

#endif /* KF_LVGL_PORT_H */
