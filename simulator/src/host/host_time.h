/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private controls for the host clock backend.
 *
 * NOT part of the HAL. Core cannot see this header and must never be able to:
 * the ability to fast-forward time is a development affordance, and if game
 * code could reach it, the pet's ageing would stop being trustworthy.
 */

#ifndef KF_HOST_TIME_H
#define KF_HOST_TIME_H

#include <cstdint>

/* When false, kf_time_delay_us() returns immediately instead of sleeping.
 * The headless CI backend sets this so a 600-frame run takes milliseconds
 * rather than twenty seconds. Defaults to true. */
void kf_host_time_set_realtime(bool realtime);

/* Move the SIMULATED wall clock forward without waiting.
 *
 * This is the mechanism that makes offline ageing testable. When the power
 * HAL lands, its desktop implementation of deep_sleep_until() will call this,
 * and three days of a pet being switched off will take a microsecond in CI.
 * The device implementation will genuinely sleep. Same code above the HAL,
 * same result, no test-only branches in core. */
void kf_host_time_advance_wall(int64_t seconds);

/* Pin the simulated wall clock to a fixed value so runs are reproducible.
 * The headless backend uses this. */
void kf_host_time_set_wall_fixed(int64_t epoch_seconds);

/* Marks the simulated wall clock as never having been set: kf_time_wall()
 * .valid becomes false and .epoch_seconds reads 0, matching a fresh device
 * whose RTC nothing has ever written (ESP32's own esp_time.cpp starts in
 * exactly this state for real, until a DS3231 answers with OSF clear).
 * kf_time_init() seeds this backend's g_wall_valid TRUE by default (see its
 * own comment), so nothing before this call can reach "clock never set" on
 * desktop -- this hook exists purely so a headless check can reach that
 * state on demand, for kf.time()'s documented "--:-- --" behaviour (Task 4,
 * docs/superpowers/plans/2026-08-13-screens-clock-sleep.md). Callers that
 * need a valid clock again afterwards call kf_host_time_set_wall_fixed(). */
void kf_host_time_set_wall_unset(void);

#endif /* KF_HOST_TIME_H */
