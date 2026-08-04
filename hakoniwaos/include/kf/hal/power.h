/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: power.
 *
 * This slice is one call: sleeping until a wall-clock time, which is what
 * lets the pet age while switched off. See kf/hal/time.h for the two clocks
 * this depends on, and docs/architecture/adr-0012-storage-and-power.md for
 * why sleep is a HAL call at all rather than something core just does with
 * kf_time_delay_us().
 *
 * NOT in this slice, on purpose (see the ADR's "Later" section): light sleep
 * between frames, wake-on-button GPIO configuration, battery voltage, and
 * charging state. Those are real and eventually necessary; none of them are
 * what the save-then-offline-age proof needs, so none of them are guessed at
 * here.
 *
 * Valid C.
 */

#ifndef KF_HAL_POWER_H
#define KF_HAL_POWER_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_POWER_VERSION 1

kf_result kf_power_init(void);

/* Sleep until the wall clock (kf/hal/time.h) reaches `wake_at`.
 *
 * READ THIS BEFORE CALLING IT: on the device, this may not return in the way
 * a normal function call does. Real deep sleep on this chip powers RAM down;
 * the chip resets and re-runs from boot, with esp_sleep_get_wakeup_cause()
 * (wrapped by a future kf_power_wake_reason(), not yet written) telling
 * kf_app_init() why it is starting. Anything the caller needs after waking
 * must already be on the other side of the reset -- which in practice means
 * saved via kf/hal/storage.h BEFORE this call, not held in a local variable
 * across it. Code that puts required work after this call and expects it to
 * run is a bug that will pass on desktop and fail on hardware.
 *
 * The desktop and headless backends DO return normally, because there is no
 * separate boot stage to simulate: they advance the simulated wall clock
 * (see simulator/src/host/host_time.h) instantly and return, which is what
 * makes three days of offline ageing a millisecond in an automated test
 * rather than three days of waiting.
 *
 * KF_ERR_INVALID if `wake_at.valid` is false -- there is no meaningful
 * "sleep until" a time the wall clock does not have. Returns KF_OK
 * immediately, without sleeping, if `wake_at` is already now or in the
 * past. */
kf_result kf_power_deep_sleep_until(kf_wall_time wake_at);

void kf_power_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_POWER_H */
