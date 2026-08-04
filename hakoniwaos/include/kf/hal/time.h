/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: time.
 *
 * TWO CLOCKS. They are not interchangeable and conflating them is the single
 * most likely way to break the pet.
 *
 *   kf_time_mono_us()  Monotonic microseconds since boot. Never jumps, never
 *                      goes backwards, means nothing outside this power
 *                      cycle. Use for frame timing, animation, timeouts.
 *
 *   kf_time_wall()     Unix epoch seconds from the RTC. Survives power off,
 *                      which is the whole point, but is adversarial: it can
 *                      be unset on first boot, jump an hour when a timezone
 *                      is corrected, jump years when a user sets it, and go
 *                      backwards when the coin cell dies. Use for the pet's
 *                      offline ageing, and never trust it without checking.
 *
 * If you use the wall clock for frame timing you get a pet that stutters when
 * NTP corrects. If you use the monotonic clock for ageing you get a pet that
 * never ages while switched off, which is the product.
 *
 * Valid C.
 */

#ifndef KF_HAL_TIME_H
#define KF_HAL_TIME_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_TIME_VERSION 1

kf_result kf_time_init(void);

/* Microseconds since boot. Monotonic. Must not wrap within any plausible
 * uptime: 64 bits of microseconds is about 584,000 years. */
uint64_t kf_time_mono_us(void);

/* Current wall-clock time. Check .valid before using .epoch_seconds.
 *
 * On the simulator this is a SIMULATED clock, not the host clock, so that
 * kf_power_deep_sleep_until() can advance it instantly and offline ageing can
 * be tested in CI without waiting three days. It starts at the host clock and
 * then runs on its own. */
kf_wall_time kf_time_wall(void);

/* Set the wall clock. Used by whatever configures the RTC (a settings screen,
 * an NTP sync, a companion app). Returns KF_ERR_UNAVAILABLE on backends with
 * a read-only clock. */
kf_result kf_time_set_wall(int64_t epoch_seconds);

/* Give the rest of the system a chance to run for roughly this long.
 *
 * NOT a power-saving sleep: that is kf/hal/power.h, which does not exist yet.
 * This is the "I have finished this frame early" yield. On the device it will
 * be a FreeRTOS delay; on desktop, a sleep; under Emscripten it must be a
 * no-op because the browser owns the schedule.
 *
 * May return early. May overshoot. Never a timing guarantee. */
void kf_time_delay_us(uint32_t microseconds);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_TIME_H */
