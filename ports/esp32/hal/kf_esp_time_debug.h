/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * One debug-only accessor into esp_time.cpp's DS3231 driver, for
 * ports/esp32/main/kf_dbg_bridge.cpp's `KFDBG RTC` command (Task 5 of
 * the screens/clock/sleep plan, finally built).
 *
 * Deliberately not a duplicate of the register map. esp_time.cpp already
 * owns the DS3231's registers, the BCD<->epoch conversion, and the
 * MPU-6050-at-the-same-address disambiguation (kf_time_init()'s own header
 * comment) -- this header exposes exactly one more read of state that file
 * already tracks (g_rtc_dev, live registers), not a second implementation
 * of any of it. If the register map ever needs to change, this function's
 * body is the only place inside esp_time.cpp that needs to change with it;
 * nothing outside that file knows the map exists.
 */

#ifndef KF_ESP_TIME_DEBUG_H
#define KF_ESP_TIME_DEBUG_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the DS3231's time and status registers directly over I2C, RIGHT
 * NOW -- NOT kf_time_wall() (kf/hal/time.h), which returns the in-RAM clock
 * last synced from the chip at boot or at the last kf_time_set_wall() call
 * and does not itself touch the bus. That distinction is the entire reason
 * `KFDBG RTC` exists rather than just wiring kf_time_wall() into a new
 * command: reading the RAM clock twice can never prove the chip and the RAM
 * copy haven't drifted apart, or that the chip is even still there --
 * see kf_dbg_bridge.cpp's handle_rtc().
 *
 * Returns false, leaving *epoch_seconds and *osf untouched, if no DS3231
 * ever answered at boot (esp_time.cpp's try_init_ds3231() never set
 * g_rtc_dev) or if either register read fails on the bus right now.
 *
 * Returns true whenever a chip answered and both reads just succeeded, EVEN
 * IF *osf comes back true (oscillator-stopped: the chip does not trust its
 * own registers -- dead/missing backup cell, or never seeded). Reporting
 * that as a distinct, diagnosable outcome -- "the chip answered but says
 * its own time is invalid" -- rather than folding it into the same false
 * a missing chip returns is the whole point of surfacing osf here instead
 * of silently refusing: it is what makes the coin-cell-removed bench test
 * (Task 5's still-open negative case) legible over the wire. */
bool kf_esp_time_debug_read_rtc(int64_t *epoch_seconds, bool *osf);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_ESP_TIME_DEBUG_H */
