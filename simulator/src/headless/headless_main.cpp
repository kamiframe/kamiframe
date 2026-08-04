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
 *     kamiframe-headless --verify-pet
 *     kamiframe-headless --verify-lua-pet
 *     kamiframe-headless --verify-pet-screen [--expect-checksum HEX]
 *     kamiframe-headless --verify-lua-creature
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
#include "kf/pet.h"
#include "../host/host_storage.h"
#include "../host/host_time.h"
#include "../lvgl/kf_lvgl_port.h"
#include "../lvgl/kf_lvgl_proof_screen.h"
#include "../lvgl/kf_pet_screen.h"
#include "../lua/kf_lua_demo_creature_script.h"
#include "../lua/kf_lua_pet_proof_script.h"
#include "../lua/kf_lua_port.h"
#include "../lua/kf_lua_proof_script.h"
#include "../pet/kf_pet_session.h"
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

/* Proves the pet simulation framework's offline fast-forward mechanism --
 * not just the decay maths in isolation, but the actual save/sleep/reload
 * path a real device goes through -- behaves identically to a single direct
 * kf_pet_advance() call. See docs/architecture/adr-0015-pet-simulation-
 * framework.md.
 *
 * Uses the same isolated-per-PID storage directory trick as
 * run_storage_power_check(), and the same kf_power_deep_sleep_until()
 * instant-fast-forward as that check's offline-ageing half -- this is that
 * same mechanism, one layer up, proving core's pet.* sits correctly on top
 * of it rather than re-proving the HAL underneath it. */
int run_pet_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-pet-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    const kf_pet_config config = kf_pet_default_config();

    /* 1. Decay math: a fresh pet is full; advancing by exactly one hour
     * must drop each need by exactly its configured mp_per_hour rate --
     * the point of computing decay as one closed-form step rather than
     * simulating it second by second. */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.happiness_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.energy_mp == KF_PET_MILLIPERCENT_MAX,
              "a fresh pet starts at full needs");

        kf_pet_advance(&state, &config, 3600u);
        check(state.hunger_mp ==
                  KF_PET_MILLIPERCENT_MAX - config.hunger_decay_mp_per_hour,
              "hunger decays by exactly the configured rate over one hour");
        check(state.happiness_mp == KF_PET_MILLIPERCENT_MAX -
                                         config.happiness_decay_mp_per_hour,
              "happiness decays by exactly the configured rate over one "
              "hour");
        check(state.energy_mp ==
                  KF_PET_MILLIPERCENT_MAX - config.energy_decay_mp_per_hour,
              "energy decays by exactly the configured rate over one hour");
    }

    /* 2. Clamp at zero: a huge elapsed time must not underflow past empty.
     * A year is nowhere near kf_pet_advance()'s real ceiling (see its
     * uint64_t intermediate math), just comfortably past every configured
     * decay rate's own "empty" point. */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        kf_pet_advance(&state, &config, 365u * 24u * 3600u);
        check(state.hunger_mp == 0u && state.happiness_mp == 0u &&
                  state.energy_mp == 0u,
              "needs clamp at zero rather than underflowing over a very "
              "long elapsed time");
    }

    /* 3. Care actions raise a need and clamp at MAX rather than banking
     * overfeeding against future decay. */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        kf_pet_feed(&state);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX,
              "feeding an already-full pet does not overflow past max");

        kf_pet_advance(&state, &config, 3600u);
        kf_pet_feed(&state);
        kf_pet_play(&state);
        kf_pet_rest(&state);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.happiness_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.energy_mp == KF_PET_MILLIPERCENT_MAX,
              "care actions top a decayed need back up to max (the boost "
              "exceeds one hour's decay for every configured rate)");
    }

    /* 4. The offline fast-forward equivalence -- the strongest proof of
     * the whole mechanism, and the actual hardware-purchase trigger:
     * save state, fast-forward the wall clock by 3 simulated days via
     * kf_power_deep_sleep_until() (exactly as run_storage_power_check()'s
     * offline-ageing half does), reload via kf_pet_load_and_advance(), and
     * confirm the result is byte-for-byte identical to calling
     * kf_pet_advance() directly on a snapshot of the pre-sleep state with
     * the same elapsed seconds. If save/sleep/reload ever drifted from
     * direct advance, this is what would catch it. */
    {
        kf_pet_state state{};
        check(kf_pet_load_and_advance(&state, &config) == KF_OK,
              "load_and_advance with no prior save succeeds");
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX,
              "a missing save initialises a fresh pet rather than erroring");
        check(state.last_advanced.valid,
              "last_advanced becomes valid after the first "
              "load_and_advance");

        /* Some care and a bit of "live" elapsed time first, so the
         * pre-sleep snapshot below is not just a fresh pet -- proving the
         * equivalence holds from an arbitrary state, not only from zero. */
        kf_pet_advance(&state, &config, 2u * 3600u);
        kf_pet_feed(&state);
        check(kf_pet_save(&state) == KF_OK, "save after some care actions");

        const kf_pet_state pre_sleep = state;

        const kf_wall_time before = kf_time_wall();
        check(before.valid, "wall clock valid before the simulated sleep");
        constexpr int64_t kThreeDaysSeconds = 3 * 24 * 60 * 60;
        kf_wall_time wake_at = before;
        wake_at.epoch_seconds += kThreeDaysSeconds;
        check(kf_power_deep_sleep_until(wake_at) == KF_OK,
              "deep sleep call (simulated 3 days offline)");

        kf_pet_state loaded{};
        check(kf_pet_load_and_advance(&loaded, &config) == KF_OK,
              "load_and_advance after the simulated offline sleep");

        kf_pet_state expected = pre_sleep;
        kf_pet_advance(&expected, &config,
                        static_cast<uint32_t>(kThreeDaysSeconds));

        check(loaded.hunger_mp == expected.hunger_mp &&
                  loaded.happiness_mp == expected.happiness_mp &&
                  loaded.energy_mp == expected.energy_mp,
              "offline fast-forward (save, sleep, load_and_advance) "
              "produces needs identical to one direct kf_pet_advance() "
              "call with the same elapsed seconds");
        check(loaded.last_advanced.valid &&
                  loaded.last_advanced.epoch_seconds == wake_at.epoch_seconds,
              "last_advanced adopts the new wall-clock reading after "
              "fast-forwarding");
    }

    /* 5. Backwards clock: an RTC reset must not age the pet negatively.
     * kf_power_deep_sleep_until() itself refuses to move time backwards (a
     * past wake_at is a no-op, see kf/hal/power.h), so this uses
     * kf_time_set_wall() directly to simulate the clock actually jumping
     * back -- the case kf_pet_load_and_advance()'s own header comment
     * documents. */
    {
        kf_pet_state state{};
        check(kf_pet_load_and_advance(&state, &config) == KF_OK,
              "load_and_advance to establish a baseline before the "
              "backwards jump");
        check(kf_pet_save(&state) == KF_OK, "save the baseline state");
        const kf_pet_state before_backwards = state;

        const kf_wall_time now = kf_time_wall();
        check(kf_time_set_wall(now.epoch_seconds - 1000) == KF_OK,
              "set the wall clock backwards by 1000s (simulated RTC "
              "reset)");

        kf_pet_state after_backwards{};
        check(kf_pet_load_and_advance(&after_backwards, &config) == KF_OK,
              "load_and_advance survives a backwards clock jump rather "
              "than erroring");
        check(after_backwards.hunger_mp == before_backwards.hunger_mp &&
                  after_backwards.happiness_mp ==
                      before_backwards.happiness_mp &&
                  after_backwards.energy_mp == before_backwards.energy_mp,
              "a backwards clock jump does not age the pet negatively -- "
              "elapsed clamps to zero rather than underflowing");
    }

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

/* Proves the pet screen (ADR 0017) renders deterministically, the same
 * property run_lvgl_check() above proves for the (separate, still in
 * place) proof screen. Bypasses kf_app_init()/kf_app_frame() entirely, the
 * same way run_lvgl_check() does -- this exercises the pet screen
 * directly, not the placeholder demo, and does not touch
 * KAMIFRAME_LVGL_GOLDEN_CHECKSUM.
 *
 * Needs more brought up first than run_lvgl_check() does: the pet screen
 * reads kf_pet_session_state(), which needs the storage/power HAL and
 * kf_pet_session_init() -- the same isolated-per-PID storage directory
 * trick run_pet_check() and run_lua_pet_check() already use. */
int run_pet_screen_check(unsigned long long expect_checksum,
                          bool have_expect) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-pet-screen-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    /* Arenas and the framebuffer are core's, not LVGL's -- see
     * run_lvgl_check()'s identical comment above. */
    kf_arena_init_all();
    kf_fb_init();

    kf_pet_session_init();

    kf_lvgl_port_init();
    kf_pet_screen_init();

    /* Same synthetic, fixed per-frame clock and frame count as
     * run_lvgl_check(), for the same reason: this loop runs flat out, so
     * real time between iterations would be scheduler noise, not frame
     * count. kf_pet_session_frame() runs first each iteration so the
     * screen update that follows draws THIS frame's state, matching the
     * ordering sdl_main.cpp uses for the interactive build. */
    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);
    for (int i = 0; i < 30; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_pet_screen_update();
        kf_lvgl_port_pump(kFixedDtMs);
    }

    /* Same FNV-1a-over-the-framebuffer approach as run_lvgl_check(), for
     * the same reason: this is fundamentally a rendering determinism
     * check, and every other rendering check in this codebase already
     * uses a checksum rather than an exact arithmetic invariant (contrast
     * run_lua_check()/run_lua_pet_check(), which are logic checks, not
     * rendering ones). */
    uint64_t checksum = 1469598103934665603ull;
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(kf_fb_pixels());
    for (size_t i = 0; i < KF_FRAMEBUFFER_BYTES; ++i) {
        checksum ^= bytes[i];
        checksum *= 1099511628211ull;
    }

    kf_lvgl_port_shutdown();
    kf_pet_session_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("checksum %016llx\n",
                static_cast<unsigned long long>(checksum));

    if (have_expect && checksum != expect_checksum) {
        KF_LOGE(TAG,
                "checksum mismatch: got %016llx, expected %016llx. The "
                "pet screen changed. If that was deliberate, update "
                "KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM in "
                "simulator/CMakeLists.txt.",
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

/* Proves the pet.* Lua binding (ADR 0016) -- not the pet decay maths
 * itself, already proved by run_pet_check() above, but the FFI wiring
 * between a Lua script and the live kf_pet_session state. Two proof
 * scripts run back to back against the SAME continuing session (see
 * kf_lua_pet_proof_script.h for why two, not one): a decay-and-read phase,
 * then a care-and-mutate phase. */
int run_lua_pet_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-lua-pet-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_arena_init_all();
    kf_pet_session_init();

    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);
    /* kf_pet_session_frame() only applies decay once
     * KF_PET_SESSION_FLUSH_SECONDS of live time has accumulated (see its
     * header comment) -- at kFixedDtMs per frame, that first flush lands
     * around frame ceil(30000/kFixedDtMs). 1200 frames clears that with
     * comfortable margin (guarantees exactly one flush, not zero) while
     * staying nowhere near a second one, so the expected direction of the
     * check below (strictly less than max, not equal to some exact
     * value) does not depend on hitting the boundary exactly. */
    constexpr long kStage1Frames = 1200;
    constexpr long kStage2Frames = 30;

    /* Stage 1: decay-and-read. No care calls at all -- just enough live
     * frames for hunger to visibly move (a fresh pet at
     * kf_pet_default_config()'s ~1042 mp/hour decays a handful of mp once
     * the flush threshold is crossed, comfortably nonzero and comfortably
     * short of exhausting the need entirely). */
    check(kf_lua_port_init(kKfLuaPetDecayProofScriptSource,
                            kKfLuaPetDecayProofScriptChunkName),
          "stage 1 (decay) proof script loaded");
    for (long i = 0; i < kStage1Frames; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_lua_port_frame(kFixedDtMs);
    }
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kStage1Frames),
          "stage 1 ran the full frame count without a script error");
    const int64_t reported_after_decay = kf_lua_port_last_report();
    const kf_pet_millipercent live_after_decay =
        kf_pet_session_state()->hunger_mp;
    kf_lua_port_shutdown();

    check(reported_after_decay == static_cast<int64_t>(live_after_decay),
          "pet.hunger() read via Lua matches kf_pet_session_state() read "
          "directly from C++ -- the FFI marshaling is exact, not just "
          "close");
    check(live_after_decay < KF_PET_MILLIPERCENT_MAX,
          "hunger visibly decayed over stage 1's live elapsed time -- "
          "proves the binding reads a genuinely live, ticking state, not "
          "a frozen snapshot taken once at registration");

    /* Stage 2: care-and-mutate, continuing the SAME kf_pet_session (it is
     * process-global, independent of the Lua VM's own lifetime -- shutting
     * down and re-initialising Lua between stages does not touch it). Care
     * every frame should keep every need pinned at max, since the boost
     * vastly exceeds one frame's decay. */
    check(kf_lua_port_init(kKfLuaPetCareProofScriptSource,
                            kKfLuaPetCareProofScriptChunkName),
          "stage 2 (care) proof script loaded");
    for (long i = 0; i < kStage2Frames; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_lua_port_frame(kFixedDtMs);
    }
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kStage2Frames),
          "stage 2 ran the full frame count without a script error");
    check(kf_lua_port_last_report() ==
              static_cast<int64_t>(KF_PET_MILLIPERCENT_MAX),
          "pet.feed() called from Lua brought hunger back to exactly max, "
          "reported back through pet.hunger()");
    const kf_pet_state *live_after_care = kf_pet_session_state();
    check(live_after_care->hunger_mp == KF_PET_MILLIPERCENT_MAX &&
              live_after_care->happiness_mp == KF_PET_MILLIPERCENT_MAX &&
              live_after_care->energy_mp == KF_PET_MILLIPERCENT_MAX,
          "pet.play() and pet.rest(), not just pet.feed(), are correctly "
          "wired -- happiness and energy are also back at max, checked "
          "directly against the live C++ session state, independent of "
          "what the script itself reported");
    kf_lua_port_shutdown();

    kf_pet_session_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the demo creature (ADR 0018) -- the actual script sdl_main.cpp
 * loads for a person to look at, not a proof script -- survives a full,
 * realistic lifecycle without ever raising a Lua runtime error: fresh and
 * full, decaying all the way down through every band to critical/empty,
 * cared back up to full again. Not a check on the exact log text: kf_log
 * (kf_lua_port.cpp's lua_kf_log) writes straight to kf_log/stderr with no
 * capture hook, so what this proves is narrower but still real -- the
 * script's own band-crossing logic (kf_lua_demo_creature_script.h's
 * `announce()`) runs correctly, in both directions, without an unhandled
 * Lua error ever disabling it (kf_lua_port_frame()'s own "disable further
 * calls" fallback on a script error, see kf_lua_port.cpp). If a future
 * edit to the script's threshold table introduced a bug -- a typo in a
 * message key, say -- this is what would catch it. */
int run_lua_creature_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-lua-creature-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_arena_init_all();
    kf_pet_session_init();

    check(kf_lua_port_init(kKfLuaDemoCreatureScriptSource,
                            kKfLuaDemoCreatureScriptChunkName),
          "demo creature script loaded");

    /* Stage 1: decay from full to empty. A day of elapsed pet-time per
     * call, applied in one kf_pet_advance() call each (kf_pet_session_
     * frame() batches whatever it's handed, see its own header comment --
     * a single huge dt is not a special case for it). 10 days comfortably
     * exhausts even the slowest of the three configured decay rates (see
     * kf_pet_default_config(), ADR 0015): every need should be clamped at
     * exactly zero by the end, which the check below confirms directly
     * against the live C++ state -- not because the demo creature script
     * needs that number, but so stage 2's "recovered to full" transition
     * below is known to start from genuine rock bottom, not merely "low". */
    constexpr uint32_t kOneDayMs = 24u * 60u * 60u * 1000u;
    constexpr long kStage1Frames = 10;
    for (long i = 0; i < kStage1Frames; ++i) {
        kf_pet_session_frame(kOneDayMs);
        kf_lua_port_frame(kOneDayMs);
    }
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kStage1Frames),
          "stage 1 (decay through every band) ran the full frame count "
          "without a script error");
    const kf_pet_state *after_decay = kf_pet_session_state();
    check(after_decay->hunger_mp == 0u && after_decay->happiness_mp == 0u &&
              after_decay->energy_mp == 0u,
          "10 days of elapsed time clamps every need to exactly zero -- "
          "confirms stage 2 below starts from genuine rock bottom");

    /* Stage 2: care back up to full, directly in C++ (not via the script --
     * this demo creature only reads, see kf_lua_demo_creature_script.h),
     * then a few more frames so on_frame() observes and announces the
     * "full" transition too, in the other direction from stage 1. Each
     * care action only boosts its need by kCareBoostMp (25000, a quarter
     * of the full range, see kf/pet.cpp) -- calibrated in ADR 0015 to
     * exceed a single hour's decay, not to refill from zero in one call.
     * Coming from stage 1's genuine rock bottom, that takes 4 calls per
     * need to exactly reach max (4 * 25000 == KF_PET_MILLIPERCENT_MAX);
     * kf_pet_feed()/play()/rest() clamp at max regardless, so a few extra
     * calls past that is a harmless margin, not a source of drift. */
    for (int i = 0; i < 5; ++i) {
        kf_pet_session_feed();
        kf_pet_session_play();
        kf_pet_session_rest();
    }

    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);
    constexpr long kStage2Frames = 3;
    for (long i = 0; i < kStage2Frames; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_lua_port_frame(kFixedDtMs);
    }
    constexpr long kTotalFrames = kStage1Frames + kStage2Frames;
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kTotalFrames),
          "stage 2 (recovery to full) ran without a script error either -- "
          "the whole round trip, both directions, raised nothing");
    const kf_pet_state *after_care = kf_pet_session_state();
    check(after_care->hunger_mp == KF_PET_MILLIPERCENT_MAX &&
              after_care->happiness_mp == KF_PET_MILLIPERCENT_MAX &&
              after_care->energy_mp == KF_PET_MILLIPERCENT_MAX,
          "feed()/play()/rest() brought every need back to exactly max");

    kf_lua_port_shutdown();
    kf_pet_session_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

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
    bool verify_pet = false;
    bool verify_lua_pet = false;
    bool verify_pet_screen = false;
    bool verify_lua_creature = false;
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
        } else if (std::strcmp(argv[i], "--verify-pet") == 0) {
            verify_pet = true;
        } else if (std::strcmp(argv[i], "--verify-lua-pet") == 0) {
            verify_lua_pet = true;
        } else if (std::strcmp(argv[i], "--verify-pet-screen") == 0) {
            verify_pet_screen = true;
        } else if (std::strcmp(argv[i], "--verify-lua-creature") == 0) {
            verify_lua_creature = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("kamiframe-headless [--frames N] [--seed N] "
                        "[--expect-checksum HEX] [--max-dirty-percent N] [--stress]\n"
                        "kamiframe-headless --verify-storage-power\n"
                        "kamiframe-headless --verify-lvgl [--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua [--frames N]\n"
                        "kamiframe-headless --verify-pet\n"
                        "kamiframe-headless --verify-lua-pet\n"
                        "kamiframe-headless --verify-pet-screen "
                        "[--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua-creature\n");
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

    if (verify_pet) {
        return run_pet_check();
    }

    if (verify_lua_pet) {
        return run_lua_pet_check();
    }

    if (verify_pet_screen) {
        return run_pet_screen_check(expect_checksum, have_expect);
    }

    if (verify_lua_creature) {
        return run_lua_creature_check();
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
