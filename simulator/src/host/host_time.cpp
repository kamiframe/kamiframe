/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: time, host implementation.
 *
 * Two clocks, kept genuinely separate, because the device has two and they
 * behave differently. See kf/hal/time.h for why conflating them breaks the
 * pet.
 *
 * The wall clock here is SIMULATED rather than a passthrough to the host
 * clock. It starts at the host's time and then runs on its own, so that
 * fast-forwarding it (host_time.h) does not require lying to the operating
 * system, and so a headless run can pin it and get identical output.
 */

#include "kf/hal/time.h"

#include "host_time.h"

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point g_boot;
bool g_started = false;

int64_t g_wall_at_boot = 0;
bool g_wall_valid = false;
bool g_wall_pinned = false;
int64_t g_wall_offset = 0; /* accumulated fast-forward, seconds */

bool g_realtime = true;

} // namespace

kf_result kf_time_init(void) {
    g_boot = Clock::now();
    g_started = true;

    if (!g_wall_pinned) {
        const auto sys = std::chrono::system_clock::now().time_since_epoch();
        g_wall_at_boot =
            std::chrono::duration_cast<std::chrono::seconds>(sys).count();
        g_wall_valid = true;
    }
    return KF_OK;
}

uint64_t kf_time_mono_us(void) {
    if (!g_started) {
        return 0u;
    }
    const auto delta = Clock::now() - g_boot;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
}

kf_wall_time kf_time_wall(void) {
    kf_wall_time out{};
    out.valid = g_wall_valid;
    if (!g_wall_valid) {
        out.epoch_seconds = 0;
        return out;
    }
    if (g_wall_pinned) {
        /* Pinned: only explicit advances move it. Deterministic. */
        out.epoch_seconds = g_wall_at_boot + g_wall_offset;
    } else {
        const uint64_t up_s = kf_time_mono_us() / 1000000ull;
        out.epoch_seconds =
            g_wall_at_boot + static_cast<int64_t>(up_s) + g_wall_offset;
    }
    return out;
}

kf_result kf_time_set_wall(int64_t epoch_seconds) {
    const kf_wall_time current = kf_time_wall();
    g_wall_offset += epoch_seconds - current.epoch_seconds;
    g_wall_valid = true;
    return KF_OK;
}

void kf_time_delay_us(uint32_t microseconds) {
    if (!g_realtime || microseconds == 0u) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

void kf_host_time_set_realtime(bool realtime) { g_realtime = realtime; }

void kf_host_time_advance_wall(int64_t seconds) { g_wall_offset += seconds; }

void kf_host_time_set_wall_fixed(int64_t epoch_seconds) {
    g_wall_pinned = true;
    g_wall_valid = true;
    g_wall_at_boot = epoch_seconds;
    g_wall_offset = 0;
}
