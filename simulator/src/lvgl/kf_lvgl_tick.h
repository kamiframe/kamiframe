/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * LVGL's tick source.
 *
 * lv_tick_inc() is the ONLY thing that advances LVGL's internal clock --
 * LVGL never reads a wall clock itself (see lv_tick.c). That makes it safe
 * to drive from either of this project's two frame loops without LVGL ever
 * knowing which one it's in, the same property kf/hal/time.h's two clocks
 * exist to protect everywhere else.
 *
 * Two callers, two honest sources, matching ADR 0011's debounce-timing
 * lesson exactly:
 *
 *   kf_lvgl_tick_advance_real()       SDL: real elapsed host time, once per
 *                                     drawn frame. Correct for a build that
 *                                     is genuinely watching the clock.
 *
 *   kf_lvgl_tick_advance_synthetic()  Headless: a caller-supplied fixed
 *                                     delta, never the host clock. Feeding
 *                                     LVGL real elapsed time in a loop that
 *                                     runs flat-out (kf_host_time_set_realtime
 *                                     (false)) would make its animations and
 *                                     redraw timing depend on how fast this
 *                                     particular machine executes the loop --
 *                                     exactly the bug ADR 0011 fixed for
 *                                     button debounce, for the same reason.
 */

#ifndef KF_LVGL_TICK_H
#define KF_LVGL_TICK_H

#include <cstdint>

/* Call once, after lv_init(). Records the starting point for
 * kf_lvgl_tick_advance_real(). */
void kf_lvgl_tick_init(void);

/* SDL: advance LVGL's clock by the real time elapsed (in ms) since the last
 * call (or since kf_lvgl_tick_init(), the first time). Uses
 * kf_time_mono_us(), the same monotonic clock every other frame-timing
 * consumer in this project uses. */
void kf_lvgl_tick_advance_real(void);

/* Headless: advance LVGL's clock by exactly `delta_ms`, regardless of how
 * long anything actually took. Callers pass a fixed nominal frame period
 * (see KF_FRAME_BUDGET_US in kf/budget.h), the same synthetic-clock pattern
 * headless_input.cpp already uses for button debounce. */
void kf_lvgl_tick_advance_synthetic(uint32_t delta_ms);

#endif /* KF_LVGL_TICK_H */
