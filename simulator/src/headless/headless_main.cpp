/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Headless runner. This is what CI executes.
 *
 * It runs the real firmware: the same arenas, the same blitter, the same
 * frame loop, the same budget accounting. Only the bottom layer differs.
 * If this passes and the SDL build passes, the HAL boundary is doing its job.
 *
 * Usage:
 *     kamiframe-headless [--frames N] [--seed N] [--expect-checksum HEX]
 *                        [--max-dirty-percent N]
 *
 * Exit codes:
 *     0  everything asserted held
 *     1  a check failed
 */

#include "kf/app.h"
#include "kf/budget.h"
#include "kf/hal/log.h"
#include "../host/host_time.h"
#include "headless_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void kf_host_entropy_pin(uint32_t value);

namespace {
constexpr const char *TAG = "headless";
}

int main(int argc, char *argv[]) {
    long frames = 300;
    unsigned long seed = 0x5EEDCAFEul;
    unsigned long long expect_checksum = 0;
    bool have_expect = false;
    long max_dirty_percent = -1;
    kf_demo_mode mode = KF_DEMO_SPRITE;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoul(argv[++i], nullptr, 0);
        } else if (std::strcmp(argv[i], "--expect-checksum") == 0 &&
                   i + 1 < argc) {
            expect_checksum = std::strtoull(argv[++i], nullptr, 16);
            have_expect = true;
        } else if (std::strcmp(argv[i], "--max-dirty-percent") == 0 &&
                   i + 1 < argc) {
            max_dirty_percent = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "--stress") == 0) {
            mode = KF_DEMO_FULLSCREEN;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("kamiframe-headless [--frames N] [--seed N] "
                        "[--expect-checksum HEX] [--max-dirty-percent N] [--stress]\n");
            return 0;
        }
    }

    /* Determinism: fixed entropy, and a pinned wall clock so nothing depends
     * on what day CI happens to run. */
    kf_host_entropy_pin(static_cast<uint32_t>(seed));
    kf_host_time_set_wall_fixed(1767225600); /* 2026-01-01T00:00:00Z */

    /* Do not sleep out the frame budget: a 300-frame run should take
     * milliseconds, not ten seconds. */
    kf_host_time_set_realtime(false);

    kf_app_init(mode);
    for (long i = 0; i < frames; ++i) {
        if (!kf_app_frame()) {
            break;
        }
    }

    const uint64_t checksum = kf_headless_checksum();
    const uint64_t presented = kf_headless_frames();
    const uint64_t dirty_pixels = kf_headless_dirty_pixels();
    const kf_frame_summary summary = kf_app_frame_summary();

    kf_app_shutdown();

    const double mean_dirty_percent =
        presented == 0
            ? 0.0
            : (static_cast<double>(dirty_pixels) /
               static_cast<double>(presented) /
               static_cast<double>(KF_FRAMEBUFFER_PIXELS)) *
                  100.0;

    std::printf("\n");
    std::printf("frames-presented   %llu\n",
                static_cast<unsigned long long>(presented));
    std::printf("checksum           %016llx\n",
                static_cast<unsigned long long>(checksum));
    std::printf("mean-dirty-percent %.2f\n", mean_dirty_percent);
    std::printf("mean-frame-us      %u\n", summary.mean_us);
    std::printf("p99-frame-us       %u\n", summary.p99_us);
    std::printf("over-budget-frames %llu\n",
                static_cast<unsigned long long>(summary.over_budget_frames));

    int status = 0;

    if (presented != static_cast<uint64_t>(frames)) {
        KF_LOGE(TAG, "expected %ld frames, presented %llu", frames,
                static_cast<unsigned long long>(presented));
        status = 1;
    }

    if (have_expect && checksum != expect_checksum) {
        KF_LOGE(TAG,
                "checksum mismatch: got %016llx, expected %016llx. "
                "Rendering changed. If that was deliberate, update the "
                "expected value in CI.",
                static_cast<unsigned long long>(checksum),
                static_cast<unsigned long long>(expect_checksum));
        status = 1;
    }

    /* The regression this exists to catch: a change that starts redrawing the
     * whole screen every frame. It would still look correct on desktop and
     * would still halve the frame rate on hardware. */
    if (max_dirty_percent >= 0 &&
        mean_dirty_percent > static_cast<double>(max_dirty_percent)) {
        KF_LOGE(TAG,
                "mean dirty area %.2f%% exceeds the %ld%% ceiling. Something "
                "started redrawing more of the screen than it needs to. On "
                "the device that is transfer time you cannot get back.",
                mean_dirty_percent, max_dirty_percent);
        status = 1;
    }

    std::printf("%s\n", status == 0 ? "PASS" : "FAIL");
    return status;
}
