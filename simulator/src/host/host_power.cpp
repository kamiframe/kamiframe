/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: power, host implementation.
 *
 * The desktop and headless backends share this file (see
 * simulator/CMakeLists.txt): both run on a host OS with no real deep sleep
 * to enter, so both implement kf_power_deep_sleep_until() the same way, the
 * "time machine" ADR 0004 and ADR 0012 describe -- advance the SIMULATED
 * wall clock (host_time.h) instantly and return. Three days of offline pet
 * ageing then costs one function call instead of three days.
 *
 * This is deliberately not the device behaviour, and kf/hal/power.h says so:
 * on the device this call may not return at all, because entering deep sleep
 * resets the chip. Nothing here can or should simulate that reset -- it
 * would make the desktop and headless backends lie about what runs after
 * the call, which is a worse failure than not simulating it.
 */

#include "kf/hal/power.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "host_time.h"

namespace {
constexpr const char *TAG = "power";
} // namespace

kf_result kf_power_init(void) { return KF_OK; }

kf_result kf_power_deep_sleep_until(kf_wall_time wake_at) {
    if (!wake_at.valid) {
        return KF_ERR_INVALID;
    }

    const kf_wall_time now = kf_time_wall();
    if (!now.valid) {
        /* Nothing meaningful to sleep relative to. Matches kf_time_wall()'s
         * own contract: a caller must check .valid before trusting it. */
        return KF_ERR_INVALID;
    }

    const int64_t delta_s = wake_at.epoch_seconds - now.epoch_seconds;
    if (delta_s <= 0) {
        return KF_OK; /* already now or in the past: nothing to wait for */
    }

    KF_LOGI(TAG, "simulated deep sleep: advancing the wall clock %lld s",
            static_cast<long long>(delta_s));
    kf_host_time_advance_wall(delta_s);
    return KF_OK;
}

void kf_power_shutdown(void) {}
