/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: power, ESP32 implementation.
 *
 * This is the one HAL call where the device backend's behaviour is supposed
 * to look nothing like the desktop one, and kf/hal/power.h says so in its
 * own header comment: esp_deep_sleep_start() is declared noreturn in
 * esp_system.h for a real reason -- it powers RAM down and the chip resets,
 * so nothing after that call in this file, or in the caller, or anywhere
 * else in this power-on session, runs again. There is no "return normally"
 * path here the way host_power.cpp has one; faking one would be exactly the
 * lie kf/hal/power.h's header comment warns against.
 *
 * kf_time_wall() must already be valid and already reflect "now" before this
 * is called -- this file does not touch the wall clock at all, it only
 * tells the RTC hardware timer how many microseconds to count before waking
 * the chip back up from reset.
 */

#include "kf/hal/power.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include "esp_sleep.h"

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
        /* Matches kf_time_wall()'s own contract: a caller must check .valid
         * before trusting it, and there is nothing meaningful to sleep
         * relative to without it. */
        return KF_ERR_INVALID;
    }

    const int64_t delta_s = wake_at.epoch_seconds - now.epoch_seconds;
    if (delta_s <= 0) {
        return KF_OK; /* already now or in the past: nothing to wait for */
    }

    KF_LOGI(TAG, "deep sleep: %lld s, then the chip resets",
            static_cast<long long>(delta_s));

    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(delta_s) * 1000000ULL);

    /* noreturn (esp_system.h). Everything after this line in this power-on
     * session, including the caller's own stack frame, is gone the moment
     * this executes -- see this file's header comment. */
    esp_deep_sleep_start();
}

void kf_power_shutdown(void) {}
