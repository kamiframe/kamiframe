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

#include "kf/clock.h"

#include <chrono>
#include <cstdint>
#include <ctime>
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
        /* The host's LOCAL time, via the same helper the Sync Clock button
         * uses -- one source of truth, deliberately.
         *
         * This used to read std::chrono::system_clock directly, i.e. UTC,
         * which meant a simulator launched anywhere but the prime meridian
         * booted showing the wrong time. It was invisible for a long while
         * because nothing displayed the clock and nothing compared it to
         * anything; it surfaced the moment a Sync Clock button existed to
         * disagree with it. Seeding from anything other than
         * kf_host_time_system_now() reintroduces exactly that disagreement,
         * so do not "simplify" this back to a chrono call. */
        g_wall_at_boot = kf_host_time_system_now();
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

void kf_host_time_set_wall_unset(void) { g_wall_valid = false; }

int64_t kf_host_time_system_now(void) {
    /* The host's LOCAL wall clock, expressed the way this system expresses
     * every other time: as an unlabelled epoch that IS local time.
     *
     * NOT std::chrono::system_clock's raw value, which is UTC. That was the
     * first version of this function and it was wrong -- Chris, 2026-08-12:
     * "it's syncing the time to 4pm, instead of 12pm. It is currently 12pm
     * in my local time." He is UTC-4, so the sync was handing the simulator
     * a number four hours ahead of the clock on his wall. kf/clock.h is
     * explicit that there is no timezone anywhere in this system and the
     * epoch it is given IS local time; a UTC value silently violates that
     * for everyone not sitting on the prime meridian, which is why it read
     * as correct in code review and wrong on screen.
     *
     * Done by asking libc for the host's broken-down LOCAL time and feeding
     * those civil fields straight into kf_epoch_from_civil() -- the
     * project's own conversion, which by design attaches no timezone
     * meaning to anything. No offset arithmetic, and no DST guesswork:
     * localtime() already resolves DST correctly for a specific instant,
     * which is the part hand-rolled offset maths gets wrong twice a year.
     *
     * localtime_r is POSIX; MSVC has localtime_s with the arguments the
     * other way round. Same split host_storage.cpp already makes. */
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) {
        return 0;
    }
#else
    if (localtime_r(&now, &local) == nullptr) {
        return 0;
    }
#endif

    kf_civil civil{};
    civil.year = static_cast<int32_t>(local.tm_year + 1900);
    civil.month = static_cast<uint8_t>(local.tm_mon + 1);
    civil.day = static_cast<uint8_t>(local.tm_mday);
    civil.hour = static_cast<uint8_t>(local.tm_hour);
    civil.minute = static_cast<uint8_t>(local.tm_min);
    /* tm_sec can be 60 on a leap second; kf_civil has no such concept, and
     * clamping is friendlier than handing the clock module a value it does
     * not model. */
    civil.second = static_cast<uint8_t>(local.tm_sec > 59 ? 59 : local.tm_sec);

    return kf_epoch_from_civil(&civil);
}
