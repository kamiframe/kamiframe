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

#endif /* KF_HOST_TIME_H */
