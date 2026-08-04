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
 *     kamiframe-headless --verify-storage-power
 *     kamiframe-headless --verify-lvgl [--expect-checksum HEX]
 *     kamiframe-headless --verify-lua [--frames N]
 *
 * Exit codes:
 *     0  everything asserted held
 *     1  a check failed
 */

#include "kf/app.h"
#include "kf/arena.h"
#include "kf/budget.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/hal/power.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"
#include "../host/host_storage.h"
#include "../host/host_time.h"
#include "../lvgl/kf_lvgl_port.h"
#include "../lvgl/kf_lvgl_proof_screen.h"
#include "../lua/kf_lua_port.h"
#include "../lua/kf_lua_proof_script.h"
#include "headless_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define KF_GETPID _getpid
#else
#include <unistd.h>
#define KF_GETPID getpid
#endif

extern "C" void kf_host_entropy_pin(uint32_t value);

namespace {
constexpr const char *TAG = "headless";

/* Proves the actual hardware-purchase trigger from the planning docs: save
 * state survives, and offline ageing works, deterministically, in
 * milliseconds rather than the three real days it names on the device. See
 * docs/architecture/adr-0012-storage-and-power.md.
 *
 * Deliberately bypasses kf_app_init()/kf_app_frame(): this checks the
 * storage and power HAL directly, not the placeholder demo, which has
 * nothing to do with either. */
int run_storage_power_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    /* An isolated directory per run: this must never read a leftover save
     * from a previous run, or a real one, and must never leave one behind. */
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-store-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    /* 1. A fresh store has nothing under any key. */
    uint8_t buf[64];
    size_t out_bytes = 0;
    check(kf_store_read("pet", buf, sizeof(buf), &out_bytes) ==
              KF_ERR_UNAVAILABLE,
          "fresh key reads as unavailable");

    /* 2. Key and value validation match kf/budget.h's real NVS limits,
     * not a design choice made here -- see kf/hal/storage.h. */
    check(kf_store_write("way-too-long-a-key-name", "x", 1) == KF_ERR_INVALID,
          "over-length key rejected");
    check(kf_store_write("bad key!", "x", 1) == KF_ERR_INVALID,
          "key with invalid characters rejected");
    {
        uint8_t oversized[KF_STORE_MAX_VALUE_BYTES + 1] = {};
        check(kf_store_write("pet", oversized, sizeof(oversized)) ==
                  KF_ERR_INVALID,
              "oversized value rejected");
    }

    /* 3. Write, then read back byte-identical. This is the save half of the
     * hardware trigger. */
    const uint8_t pet_state[] = {0x50, 0x45, 0x54, 0x01, 0x2A, 0x00, 0x7F};
    check(kf_store_write("pet", pet_state, sizeof(pet_state)) == KF_OK,
          "write pet state");
    uint8_t readback[sizeof(pet_state)] = {};
    out_bytes = 0;
    check(kf_store_read("pet", readback, sizeof(readback), &out_bytes) ==
              KF_OK,
          "read pet state back");
    check(out_bytes == sizeof(pet_state) &&
              std::memcmp(readback, pet_state, sizeof(pet_state)) == 0,
          "pet state read back byte-identical");

    /* 4. The offline-ageing half: sleep 3 real days, instantly, and prove the
     * wall clock actually moved by exactly that much. */
    const kf_wall_time before = kf_time_wall();
    check(before.valid, "wall clock valid before sleep");
    constexpr int64_t kThreeDaysSeconds = 3 * 24 * 60 * 60;
    kf_wall_time wake_at = before;
    wake_at.epoch_seconds += kThreeDaysSeconds;
    check(kf_power_deep_sleep_until(wake_at) == KF_OK, "deep sleep call");
    const kf_wall_time after = kf_time_wall();
    check(after.valid && after.epoch_seconds - before.epoch_seconds ==
                              kThreeDaysSeconds,
          "wall clock advanced by exactly 3 simulated days");

    /* 5. Save state survives the sleep -- the actual point of both halves
     * existing together: the pet ages because it was saved before power
     * was lost, not despite it. */
    out_bytes = 0;
    check(kf_store_read("pet", readback, sizeof(readback), &out_bytes) ==
                  KF_OK &&
              std::memcmp(readback, pet_state, sizeof(pet_state)) == 0,
          "pet state survives the simulated sleep");

    /* 6. Sleeping until a time already in the past is a harmless no-op, not
     * an error -- a device whose clock drifted forward must not refuse to
     * boot. */
    kf_wall_time past = after;
    past.epoch_seconds -= 10;
    check(kf_power_deep_sleep_until(past) == KF_OK,
          "sleeping until the past is a no-op");

    /* 7. Erase removes it; reading it afterward looks exactly like it was
     * never written. */
    check(kf_store_erase("pet") == KF_OK, "erase pet state");
    out_bytes = 123u; /* poison, to prove it gets reset to 0 on this path */
    check(kf_store_read("pet", readback, sizeof(readback), &out_bytes) ==
                  KF_ERR_UNAVAILABLE &&
              out_bytes == 0u,
          "erased key reads as unavailable again");

    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the LVGL port glue -- not the custom engine -- renders
 * deterministically. See ADR 0013. Bypasses kf_app_init()/kf_app_frame()
 * entirely: this exercises LVGL directly, the same way
 * run_storage_power_check() exercises storage/power directly, and never
 * touches the golden checksums the frame-loop tests guard. */
int run_lvgl_check(unsigned long long expect_checksum, bool have_expect) {
    /* Arenas and the framebuffer are core's, not LVGL's -- LVGL only ever
     * gets memory through kf_lvgl_mem_pool_alloc(), which calls
     * kf_arena_alloc(KF_ARENA_LVGL, ...). Both must exist first. */
    kf_arena_init_all();
    kf_fb_init();

    kf_lvgl_port_init();
    kf_lvgl_proof_screen_init();

    /* A synthetic per-frame clock, never real elapsed host time -- see
     * kf_lvgl_tick.h. This loop runs flat out (kf_host_time_set_realtime
     * (false), set in main() below), so real time between iterations would
     * be scheduler noise, not frame count, the exact bug ADR 0011 fixed for
     * button debounce. 30 ticks at a nominal 30fps frame period is enough
     * for LVGL to have laid out and flushed the proof screen at least once,
     * with headroom to spare. */
    for (int i = 0; i < 30; ++i) {
        kf_lvgl_port_pump(static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u));
    }

    /* FNV-1a over the framebuffer LVGL's flush callback wrote into, the
     * same algorithm headless_display.cpp uses for the golden-frame tests,
     * computed independently here since this path never calls
     * kf_display_present(). */
    uint64_t checksum = 1469598103934665603ull;
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(kf_fb_pixels());
    for (size_t i = 0; i < KF_FRAMEBUFFER_BYTES; ++i) {
        checksum ^= bytes[i];
        checksum *= 1099511628211ull;
    }

    kf_lvgl_port_shutdown();

    std::printf("checksum %016llx\n",
                static_cast<unsigned long long>(checksum));

    bool ok = true;
    if (have_expect && checksum != expect_checksum) {
        KF_LOGE(TAG,
                "checksum mismatch: got %016llx, expected %016llx. The "
                "proof screen changed. If that was deliberate, update "
                "KAMIFRAME_LVGL_GOLDEN_CHECKSUM in simulator/CMakeLists.txt.",
                static_cast<unsigned long long>(checksum), expect_checksum);
        ok = false;
    }

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the Lua port glue -- the sandboxed VM, kf_lua_alloc's free/realloc
 * path under real churn, and both directions of the kf.* binding -- behave
 * identically frame to frame. See ADR 0014. Bypasses kf_app_init()/
 * kf_app_frame() entirely, the same way run_lvgl_check() bypasses them for
 * LVGL: this exercises Lua directly, not the placeholder demo.
 *
 * Only kf_arena_init_all() from core is needed first: kf_lua_alloc_init()
 * (called inside kf_lua_port_init()) gets its one big block from
 * KF_ARENA_LUA, and nothing else Lua touches here comes from core. */
int run_lua_check(long frames) {
    kf_arena_init_all();

    if (!kf_lua_port_init(kKfLuaProofScriptSource,
                           kKfLuaProofScriptChunkName)) {
        KF_LOGE(TAG, "FAILED: proof script did not load or its top-level "
                     "code raised an error (see the log above)");
        std::printf("FAIL\n");
        return 1;
    }

    /* Synthetic, fixed per-frame delta, never real elapsed host time -- the
     * same reasoning as run_lvgl_check()'s loop below and, further back,
     * ADR 0011's debounce-timing fix: this loop runs flat out
     * (kf_host_time_set_realtime(false), set in main() below), so real time
     * between iterations would be scheduler noise, not frame count. */
    for (long i = 0; i < frames; ++i) {
        kf_lua_port_frame(static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u));
    }

    const int64_t report = kf_lua_port_last_report();
    const uint32_t ran = kf_lua_port_frame_count();
    /* Not a golden hash, unlike run_lvgl_check()'s checksum: the proof
     * script's own arithmetic (kf_lua_proof_script.h) makes `total` after N
     * calls to on_frame exactly 32*N, an invariant this can check directly
     * rather than via a hash that would need updating every time the
     * script's wording changed. */
    const int64_t expected = 32LL * static_cast<int64_t>(frames);

    kf_lua_port_shutdown();

    std::printf("frames-ran  %u\n", ran);
    std::printf("last-report %lld\n", static_cast<long long>(report));

    bool ok = true;
    if (ran != static_cast<uint32_t>(frames)) {
        KF_LOGE(TAG,
                "expected %ld on_frame calls, got %u -- the script started "
                "erroring partway through (see the log above)",
                frames, ran);
        ok = false;
    }
    if (report != expected) {
        KF_LOGE(TAG,
                "last-report %lld != expected %lld (32 * frames). Either "
                "the proof script changed on purpose (update the 32 here "
                "and in simulator/CMakeLists.txt's comment to match) or the "
                "allocator/VM did not behave deterministically.",
                static_cast<long long>(report), static_cast<long long>(expected));
        ok = false;
    }

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[]) {
    long frames = 300;
    unsigned long seed = 0x5EEDCAFEul;
    unsigned long long expect_checksum = 0;
    bool have_expect = false;
    long max_dirty_percent = -1;
    bool verify_storage_power = false;
    bool verify_lvgl = false;
    bool verify_lua = false;
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
        } else if (std::strcmp(argv[i], "--verify-storage-power") == 0) {
            verify_storage_power = true;
        } else if (std::strcmp(argv[i], "--verify-lvgl") == 0) {
            verify_lvgl = true;
        } else if (std::strcmp(argv[i], "--verify-lua") == 0) {
            verify_lua = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("kamiframe-headless [--frames N] [--seed N] "
                        "[--expect-checksum HEX] [--max-dirty-percent N] [--stress]\n"
                        "kamiframe-headless --verify-storage-power\n"
                        "kamiframe-headless --verify-lvgl [--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua [--frames N]\n");
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

    if (verify_storage_power) {
        return run_storage_power_check();
    }

    if (verify_lvgl) {
        return run_lvgl_check(expect_checksum, have_expect);
    }

    if (verify_lua) {
        return run_lua_check(frames);
    }

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
