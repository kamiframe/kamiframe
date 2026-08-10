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
 *     kamiframe-headless --verify-pet-stage
 *     kamiframe-headless --verify-pet-personality
 *     kamiframe-headless --verify-mess
 *     kamiframe-headless --verify-dirtiness
 *     kamiframe-headless --verify-pet-preferences
 *     kamiframe-headless --verify-pet-care-variation
 *     kamiframe-headless --verify-creature-pose
 *     kamiframe-headless --verify-creature-wander
 *     kamiframe-headless --verify-creature-screen
 *     kamiframe-headless --verify-creature-screen-sprites
 *     kamiframe-headless --verify-creature-screen-input
 *     kamiframe-headless --verify-creature-screen-egg
 *     kamiframe-headless --verify-creature-screen-death
 *     kamiframe-headless --verify-creature-screen-stats
 *     kamiframe-headless --verify-lua-pet
 *     kamiframe-headless --verify-pet-screen [--expect-checksum HEX]
 *     kamiframe-headless --verify-demand-curve
 *     kamiframe-headless --verify-screen-nav [--expect-checksum HEX]
 *     kamiframe-headless --verify-lua-creature
 *     kamiframe-headless --verify-assets
 *     kamiframe-headless --verify-indexed-assets
 *     kamiframe-headless --verify-blit-mirror
 *     kamiframe-headless --verify-indexed-blit
 *     kamiframe-headless --verify-creature-anim
 *     kamiframe-headless --verify-frame-counters
 *     kamiframe-headless --verify-creature-screen-budget-combo
 *     kamiframe-headless --verify-scene
 *     kamiframe-headless --verify-lua-draw
 *
 * Exit codes:
 *     0  everything asserted held
 *     1  a check failed
 */

#include "kf/app.h"
#include "kf/arena.h"
#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/creature.h"
#include "kf/font.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/hal/power.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"
#include "kf/pet.h"
#include "kf/rng.h"
#include "kf/scene.h"
#include "../host/host_assets.h"
#include "../host/host_storage.h"
#include "../host/host_time.h"
#include "../lvgl/kf_lvgl_port.h"
#include "../lvgl/kf_lvgl_proof_screen.h"
#include "../lvgl/kf_pet_screen.h"
#include "../lvgl/kf_screen_nav.h"
#include "../../../sdk/lua/generated/kf_lua_demo_creature_script.h"
#include "../../../sdk/lua/kf_lua_alloc.h"
#include "../../../sdk/lua/kf_lua_port.h"
#include "../lua/kf_lua_pet_proof_script.h"
#include "../lua/kf_lua_proof_script.h"
#include "../pet/kf_creature_screen.h"
#include "../pet/kf_pet_session.h"
#include "headless_probe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define KF_GETPID _getpid
#else
#include <unistd.h>
#define KF_GETPID getpid
#endif

/* run_creature_screen_sprite_check()'s own asset pack -- see simulator/
 * CMakeLists.txt's KF_CREATURE_DEMO_PACK_PATH comment for why this is a
 * compile-time define private to this target rather than a new default
 * (KF_ASSET_PACK) every other test would also inherit. */
#ifndef KF_CREATURE_DEMO_PACK_PATH
#error "KF_CREATURE_DEMO_PACK_PATH must be defined by the build -- see simulator/CMakeLists.txt"
#endif

/* run_indexed_asset_check()'s own fixture pack -- same reasoning as
 * KF_CREATURE_DEMO_PACK_PATH just above: a compile-time define private to
 * this target, not a new default every other test would also inherit. */
#ifndef KF_INDEXED_FIXTURE_PACK_PATH
#error "KF_INDEXED_FIXTURE_PACK_PATH must be defined by the build -- see simulator/CMakeLists.txt"
#endif

/* run_indexed_asset_check()'s and run_indexed_blit_check()'s RGB565
 * reference. Before the animated-indexed-sprites plan's Task 3, "the
 * default pack" (examples/hello_sprite/assets.kfpack) WAS this reference --
 * both checks mounted it plainly, expecting test_sprite to come back
 * KF_SPRITE_FORMAT_RGB565. Task 3 converted the default pack to indexed, so
 * that reference moved to its own permanent, checked-in fixture holding
 * exactly what the default pack used to: the same test_sprite generator,
 * packed as ASSET_TYPE_SPRITE. Same reasoning as the two defines above: a
 * compile-time define private to this target. */
#ifndef KF_RGB565_FIXTURE_PACK_PATH
#error "KF_RGB565_FIXTURE_PACK_PATH must be defined by the build -- see simulator/CMakeLists.txt"
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

/* Test helper: zeroes every stage's decay rates, not just one. Several
 * checks below want needs held perfectly constant across a stage (or a
 * whole life spanning several stages) so the expected value is exact
 * arithmetic rather than something that has to duplicate apply_decay()'s
 * own math -- since the demand-curve plan (2026-08-09) made rates per-stage
 * instead of one flat set for the whole life, "zero decay" now means
 * zeroing every row of the table, not three fields. */
kf_pet_config zero_all_stage_rates(kf_pet_config config) {
    for (unsigned s = 0; s < KF_PET_STAGE_COUNT; ++s) {
        config.stage_rates[s] = {0u, 0u, 0u};
    }
    return config;
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
     * simulating it second by second.
     *
     * A fresh pet starts in KF_PET_STAGE_EGG (ADR 0021), which
     * deliberately does not decay at all -- "no care needed as an egg,"
     * Chris's own words. This block is testing DECAY MATH, not egg
     * behaviour (egg's own no-decay guarantee gets its own dedicated
     * check further down), so it moves the state past the egg stage
     * first by setting `stage` directly -- kf_pet_state's fields are
     * plain and already poked directly elsewhere in this file (see
     * kf_time_set_wall() in check 5 below for the identical pattern
     * applied to the wall clock instead of the pet). */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.happiness_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.energy_mp == KF_PET_MILLIPERCENT_MAX,
              "a fresh pet starts at full needs");
        state.stage = KF_PET_STAGE_BABY;

        kf_pet_advance(&state, &config, 3600u);
        const kf_pet_stage_rates &baby_rates =
            config.stage_rates[KF_PET_STAGE_BABY];
        check(state.hunger_mp ==
                  KF_PET_MILLIPERCENT_MAX - baby_rates.hunger_mp_per_hour,
              "hunger decays by exactly the configured rate over one hour");
        check(state.happiness_mp ==
                  KF_PET_MILLIPERCENT_MAX - baby_rates.happiness_mp_per_hour,
              "happiness decays by exactly the configured rate over one "
              "hour");
        check(state.energy_mp ==
                  KF_PET_MILLIPERCENT_MAX - baby_rates.energy_mp_per_hour,
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
        kf_pet_feed(&state, &config, 0u);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX,
              "feeding an already-full pet does not overflow past max");

        kf_pet_advance(&state, &config, 3600u);
        kf_pet_feed(&state, &config, 0u);
        kf_pet_play(&state, &config, 0u);
        kf_pet_rest(&state, &config, 0u);
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
        kf_pet_feed(&state, &config, 0u);
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

/* Proves the life-stages/evolution mechanic (ADR 0021), on top of
 * run_pet_check()'s existing decay/offline-ageing coverage: the egg's
 * no-decay guarantee, a single kf_pet_advance() call correctly crossing
 * FOUR real stage boundaries at once (the actual closed-form claim --
 * offline for a week should cost the same handful of steps as offline for
 * an hour), the care-integral accumulator resetting at each transition
 * rather than leaking between stages, select_branch()'s equal-band mapping
 * landing in the expected band for known care scores, Adult being terminal,
 * and the v2 save format round-tripping every new field -- including the
 * exact fallback path just fixed in kf_pet_load_and_advance() (a
 * version-1, pre-evolution save must reset to a fresh pet, not error). */
int run_pet_stage_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-pet-stage-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    const kf_pet_config config = kf_pet_default_config();

    /* 1. Egg: "just a timer, no care needed" (Chris's own words) -- needs
     * must not move at all for any elapsed time short of the full egg
     * duration, however long that elapsed time is. */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        check(state.stage == KF_PET_STAGE_EGG, "a fresh pet starts as an egg");

        kf_pet_advance(&state, &config, config.egg_duration_seconds - 1u);
        check(state.hunger_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.happiness_mp == KF_PET_MILLIPERCENT_MAX &&
                  state.energy_mp == KF_PET_MILLIPERCENT_MAX,
              "needs do not decay at all while still an egg, right up to "
              "the last second before hatching");
        check(state.stage == KF_PET_STAGE_EGG &&
                  state.stage_elapsed_seconds ==
                      config.egg_duration_seconds - 1u,
              "the egg's own timer still advances even though needs do not");
    }

    /* 2. Egg hatches into Baby, and -- proving a single call does not stop
     * dead at the stage boundary -- leftover elapsed time in the SAME call
     * continues on into Baby and decays needs there. */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        constexpr uint32_t kIntoBabySeconds = 1800u; /* 30 minutes */
        kf_pet_advance(&state, &config,
                        config.egg_duration_seconds + kIntoBabySeconds);

        check(state.stage == KF_PET_STAGE_BABY,
              "elapsed time past the egg duration hatches the pet into "
              "Baby within the same kf_pet_advance() call");
        check(state.stage_elapsed_seconds == kIntoBabySeconds,
              "the leftover time after hatching is credited to the new "
              "stage, not dropped or double-counted");
        check(state.hunger_mp ==
                  KF_PET_MILLIPERCENT_MAX -
                      static_cast<kf_pet_millipercent>(
                          static_cast<uint64_t>(
                              config.stage_rates[KF_PET_STAGE_BABY]
                                  .hunger_mp_per_hour) *
                          kIntoBabySeconds / 3600ull),
              "decay resumes correctly for the leftover time once past the "
              "egg stage, using the same closed-form rate math as ordinary "
              "decay");
    }

    /* 3. Baby's own care does not feed a branch choice, and the care
     * accumulator resets cleanly at the Baby->Child boundary rather than
     * carrying over garbage from the stage before it. Poked directly at
     * Baby, one second short of Child, with the accumulator deliberately
     * pre-set to a nonzero sentinel -- the established direct-field-poke
     * pattern this file already uses (see run_pet_check()'s check 1). */
    {
        kf_pet_state state;
        kf_pet_init(&state);
        state.stage = KF_PET_STAGE_BABY;
        state.stage_elapsed_seconds = config.baby_duration_seconds - 100u;
        state.care_integral_mp_seconds = 999999999ull; /* sentinel */

        kf_pet_advance(&state, &config, 100u); /* lands exactly on the boundary */

        check(state.stage == KF_PET_STAGE_CHILD,
              "Baby transitions into Child once its configured duration is "
              "reached");
        check(state.stage_elapsed_seconds == 0u,
              "the new stage's elapsed timer starts at zero");
        check(state.care_integral_mp_seconds == 0u,
              "the care accumulator resets at the stage boundary rather "
              "than carrying Baby's sentinel value into Child's own "
              "average (Baby's care does not feed a branch choice)");
        check(state.teen_form == 0u,
              "teen_form is not decided yet -- Child has not ended");
    }

    /* 4. select_branch()'s equal-band mapping, isolated from decay: a
     * custom config with every decay rate set to zero holds needs
     * perfectly constant for the whole stage, so the care average is
     * exactly the constant value that was set going in -- no need to
     * duplicate apply_decay()'s own arithmetic here to predict the
     * answer. Three known care levels are checked against
     * (branch_count == KF_PET_TEEN_FORM_COUNT == 4 now that the tree has
     * four verb families -- see the evolution-tree-reconciliation plan):
     * neglected lands on band 0, mediocre on band 1, well-cared-for on the
     * top band, 3. Band 2 is not hit by any case here, which is fine -- this
     * is exact arithmetic on select_branch()'s own formula
     * (average_mp * branch_count) / (MAX + 1), not a claim that every band
     * needs a dedicated case. */
    {
        kf_pet_config zero_decay = zero_all_stage_rates(config);
        zero_decay.child_duration_seconds = 1000u;

        /* 22%: above dust_care_average_mp (20%), below the bottom family
         * band's own ceiling of 25%. The window between "barely raised at
         * all" and "raised badly" is genuinely narrow, and this case is
         * about the FAMILY bands, so it has to sit inside it deliberately
         * rather than land on the dust form by accident. The dust boundary
         * itself is checked in run_hokorimaru_check(). */
        const kf_pet_millipercent kNeglected = 22000u;   /* -> band 0 */
        const kf_pet_millipercent kMediocre = 50000u;    /* 50% -> band 1 */
        const kf_pet_millipercent kWellCared = 90000u;   /* 90% -> band 3 */
        const struct {
            kf_pet_millipercent level;
            uint8_t expected_band;
            const char *label;
        } cases[] = {
            {kNeglected, 0u, "a neglected Child (10% needs, held constant) "
                              "lands on the worst teen_form band (0 of 4)"},
            {kMediocre, 1u, "a mediocre Child (50% needs, held constant) "
                             "lands on teen_form band 1 of 4"},
            {kWellCared, 3u, "a well-cared-for Child (90% needs, held "
                              "constant) lands on the best teen_form band "
                              "(3 of 4)"},
        };
        for (const auto &c : cases) {
            kf_pet_state state;
            kf_pet_init(&state);
            state.stage = KF_PET_STAGE_CHILD;
            state.hunger_mp = c.level;
            state.happiness_mp = c.level;
            state.energy_mp = c.level;
            /* This check is about select_branch()'s quality-band math, not
             * the never-touched condition (that's run_hokorimaru_check()'s
             * job) -- even "neglected" here means poorly cared for, not
             * never cared for, so care_actions_taken must be nonzero or
             * every case below would collapse onto KF_PET_TEEN_FORM_DUST
             * regardless of the care level being tested. */
            state.care_actions_taken = 1u;

            kf_pet_advance(&state, &zero_decay,
                            zero_decay.child_duration_seconds);

            check(state.stage == KF_PET_STAGE_TEEN, "Child completes into Teen");
            check(state.teen_form == c.expected_band, c.label);
            check(state.hunger_mp == c.level && state.happiness_mp == c.level &&
                      state.energy_mp == c.level,
                  "zero decay rates hold needs perfectly constant across "
                  "the stage, which is what makes the care average exactly "
                  "predictable in this check");
        }
    }

    /* 5. The full closed-form claim: ONE kf_pet_advance() call, from a
     * fresh egg, spans FOUR real stage transitions (Egg->Baby->Child->
     * Teen->Adult) plus leftover time into Adult -- exactly the "offline
     * for a week, hatched and grew up while it was off" case ADR 0021
     * names. Zero decay rates again isolate the care-band arithmetic from
     * needing to duplicate apply_decay()'s own math; both branch choices
     * (teen_form during Child, adult_branch during Teen) are checked
     * against known bands in the same call. Also proves Adult is terminal:
     * a second advance() call afterwards keeps accumulating time without
     * erroring, looping, or picking another branch. */
    {
        kf_pet_config zero_decay = zero_all_stage_rates(config);
        zero_decay.egg_duration_seconds = 100u;
        zero_decay.baby_duration_seconds = 100u;
        zero_decay.child_duration_seconds = 1000u;
        zero_decay.teen_duration_seconds = 1000u;

        kf_pet_state state;
        kf_pet_init(&state);
        state.hunger_mp = 80000u; /* 80%, held constant by zero decay */
        state.happiness_mp = 80000u;
        state.energy_mp = 80000u;
        /* Same reasoning as check 4: this is testing the branch-selection
         * arithmetic across a whole lifecycle, not the never-touched
         * condition, so it must not collapse onto KF_PET_TEEN_FORM_DUST. */
        state.care_actions_taken = 1u;

        constexpr uint32_t kLeftoverInAdult = 50u;
        const uint32_t total_elapsed = zero_decay.egg_duration_seconds +
                                        zero_decay.baby_duration_seconds +
                                        zero_decay.child_duration_seconds +
                                        zero_decay.teen_duration_seconds +
                                        kLeftoverInAdult;

        kf_pet_advance(&state, &zero_decay, total_elapsed);

        check(state.stage == KF_PET_STAGE_ADULT,
              "a single kf_pet_advance() call spanning egg+baby+child+teen+"
              "some crosses all four real stage boundaries and lands in "
              "Adult, not just the first one");
        check(state.stage_elapsed_seconds == kLeftoverInAdult,
              "time left over after the last transition is credited to "
              "Adult, the same leftover-crediting proven for Baby in check "
              "2 above");
        /* 80% average with 4 bands (KF_PET_TEEN_FORM_COUNT):
         * (80000*4)/100001 = 3 (top band) -- lands on family 3, Go, which
         * the character bible gives exactly one adult. */
        check(state.teen_form == 3u,
              "teen_form, decided from Child's 80%-constant care, lands on "
              "the top of 4 bands (family 3, Go)");
        /* Go has only 1 adult (kf_pet_adults_in_family(3) == 1u), so
         * select_branch()'s band count is 1 regardless of the 80% care
         * score: (80000*1)/100001 = 0, the only band there is. A
         * single-adult family still passes through branch selection
         * deterministically -- it just has nowhere else to land. */
        check(state.adult_branch == 0u,
              "adult_branch, decided from Teen's 80%-constant care against "
              "Go's single-adult family, lands on its only band (0)");
        check(state.hunger_mp == 80000u && state.happiness_mp == 80000u &&
                  state.energy_mp == 80000u,
              "needs are still exactly 80% -- zero decay rates held them "
              "constant through every stage, including the leftover time "
              "in Adult");

        kf_pet_advance(&state, &zero_decay, 500u);
        check(state.stage == KF_PET_STAGE_ADULT,
              "Adult is terminal -- a further advance() call does not pick "
              "another stage or branch");
        check(state.stage_elapsed_seconds == kLeftoverInAdult + 500u,
              "Adult's own elapsed timer keeps accumulating rather than "
              "resetting or being ignored");
        check(state.teen_form == 3u && state.adult_branch == 0u,
              "the branches already decided stay exactly as they were -- "
              "nothing re-picks them once in the terminal stage");
    }

    /* 6. The v2 save format round-trips every new field. The wall clock is
     * pinned to the exact instant last_advanced was set (same technique as
     * run_pet_check()'s check 5, kf_time_set_wall()) so the reload's
     * elapsed time is deterministically zero -- otherwise Teen's own
     * ongoing care accumulation would perturb care_integral_mp_seconds by
     * an amount that depends on real wall-clock timing, not on whether the
     * save format round-tripped correctly. */
    {
        kf_pet_state state{};
        check(kf_pet_load_and_advance(&state, &config) == KF_OK,
              "load_and_advance with no prior save succeeds");

        state.stage = KF_PET_STAGE_TEEN;
        state.teen_form = 1u;
        state.adult_branch = 0u; /* not decided yet at Teen */
        state.stage_elapsed_seconds = 12345u;
        state.care_integral_mp_seconds = 678900000ull;
        state.hunger_mp = 42000u;
        state.happiness_mp = 55000u;
        state.energy_mp = 77000u;

        const kf_wall_time saved_at = state.last_advanced;
        check(kf_pet_save(&state) == KF_OK, "save the non-trivial state");
        check(kf_time_set_wall(saved_at.epoch_seconds) == KF_OK,
              "pin the wall clock back to exactly the saved last_advanced, "
              "guaranteeing the reload below sees zero elapsed time");

        kf_pet_state loaded{};
        check(kf_pet_load_and_advance(&loaded, &config) == KF_OK,
              "load_and_advance reloads the save");
        check(loaded.stage == KF_PET_STAGE_TEEN &&
                  loaded.teen_form == 1u && loaded.adult_branch == 0u,
              "stage and both branch indices round-trip through save/load");
        check(loaded.stage_elapsed_seconds == 12345u &&
                  loaded.care_integral_mp_seconds == 678900000ull,
              "both new per-stage accumulators round-trip through "
              "save/load, byte for byte, with zero elapsed time to "
              "perturb them");
        check(loaded.hunger_mp == 42000u && loaded.happiness_mp == 55000u &&
                  loaded.energy_mp == 77000u,
              "the pre-existing needs fields still round-trip correctly "
              "alongside the new v2 fields");
    }

    /* 7. The bug fixed alongside the v2 save-format bump: a save written
     * by an incompatible version (or simply the wrong size) must fall back
     * to a fresh pet, not propagate an error -- kf_pet_load_and_advance()'s
     * own documented contract, which the ORIGINAL code for this bump broke
     * (it returned KF_ERR_INVALID directly, which would have crashed the
     * app via kf_pet_session_init()'s KF_ASSERT on any old or corrupt
     * save). Writes raw bytes directly via kf_store_write() -- there is no
     * public unpack() to call from here, which is the point: this check
     * exercises exactly what a real stale save on a real device would
     * produce. */
    {
        /* A version-1 (pre-evolution) save: right size, wrong version byte
         * at offset 0. The rest of the bytes do not matter -- unpack()
         * must reject on the version check before ever reading them. */
        uint8_t v1_buf[KF_PET_SAVE_BYTES] = {};
        v1_buf[0] = 1u; /* kSaveVersion is 2; this claims version 1 */
        check(kf_store_write(KF_PET_SAVE_KEY, v1_buf, sizeof(v1_buf)) == KF_OK,
              "write a synthetic version-1 save directly");

        kf_pet_state state{};
        check(kf_pet_load_and_advance(&state, &config) == KF_OK,
              "load_and_advance on a version-incompatible save returns KF_OK, "
              "not an error -- the real bug this exact path had before "
              "ADR 0021 fixed it");
        check(state.stage == KF_PET_STAGE_EGG &&
                  state.hunger_mp == KF_PET_MILLIPERCENT_MAX,
              "a rejected version-1 save falls back to a fresh pet (a new "
              "egg), exactly as kf_pet_load_and_advance()'s header comment "
              "documents");

        /* Wrong size entirely (e.g. a save from some other, unrelated
         * format bump): also refused, also falls back cleanly. */
        uint8_t wrong_size_buf[KF_PET_SAVE_BYTES - 5] = {};
        check(kf_store_write(KF_PET_SAVE_KEY, wrong_size_buf,
                              sizeof(wrong_size_buf)) == KF_OK,
              "write a synthetic wrong-size save directly");

        kf_pet_state state2{};
        check(kf_pet_load_and_advance(&state2, &config) == KF_OK,
              "load_and_advance on a wrong-size save also returns KF_OK");
        check(state2.stage == KF_PET_STAGE_EGG &&
                  state2.hunger_mp == KF_PET_MILLIPERCENT_MAX,
              "a wrong-size save also falls back to a fresh pet");
    }

    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves ADR 0023's personality traits: base_trait rolled once and never
 * touched again, the three care-derived accumulators building whole-life
 * (never resetting at a stage transition, unlike care_integral_mp_seconds
 * -- see run_pet_stage_check()'s check 3 for the contrasting reset
 * behaviour), the periodic-halving math itself, kf_pet_dominant_care_
 * trait()'s band selection, and the v3 save format round-tripping every
 * new field. Same isolated-per-PID storage directory trick as
 * run_pet_stage_check() above. */
int run_pet_personality_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-pet-personality-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    const kf_pet_config config = kf_pet_default_config();

    /* 1. base_trait: in range, and repeatable under a pinned RNG seed --
     * the same "poke a known input, check a known output" style as
     * run_pet_stage_check()'s select_branch() checks, applied to
     * kf_rng_below() instead. Two fresh pets from the identical reseed
     * must land on the identical trait; this is the actual claim doc 16
     * makes ("same RNG path... same determinism story the rest of this
     * project already has"), not just "the value is in range". */
    {
        kf_rng_seed(0x1234u);
        kf_pet_state state_a;
        kf_pet_init(&state_a);
        check(state_a.base_trait < KF_PET_BASE_TRAIT_COUNT,
              "base_trait is rolled within the base-trait table's bounds");

        kf_rng_seed(0x1234u);
        kf_pet_state state_b;
        kf_pet_init(&state_b);
        check(state_a.base_trait == state_b.base_trait,
              "the identical RNG seed produces the identical base_trait "
              "on a fresh pet -- repeatable, not merely in-range");

        /* 2. Rolled once, never touched again: neither ordinary time
         * passing nor a care action may change it. */
        kf_pet_state state = state_a;
        kf_pet_advance(&state, &config, 30u * 86400u); /* a month, offline-style */
        state.hunger_mp = 0u;
        kf_pet_feed(&state, &config, 0u);
        check(state.base_trait == state_a.base_trait,
              "base_trait is unchanged by kf_pet_advance() and a care "
              "action -- rolled once at init, fixed for the pet's whole "
              "life, per kf/pet.h's header comment");
    }

    /* 3. Whole-life accumulation: personality accumulators are NOT reset
     * at a stage transition, unlike care_integral_mp_seconds (see
     * run_pet_stage_check()'s check 3, the direct contrast). Zero decay
     * and zero personality half-life (treated as "no periodic halving",
     * per accumulate_personality()'s own header comment) isolate this
     * from both decay math and the halving math below. */
    {
        kf_pet_config zero_decay_no_halving = zero_all_stage_rates(config);
        zero_decay_no_halving.personality_recency_half_life_seconds = 0u;
        zero_decay_no_halving.baby_duration_seconds = 1000u;
        zero_decay_no_halving.child_duration_seconds = 500u;

        kf_pet_state state;
        kf_pet_init(&state);
        state.stage = KF_PET_STAGE_BABY;
        state.hunger_mp = 60000u; /* held constant by zero decay */
        state.happiness_mp = 60000u;
        state.energy_mp = 60000u;

        kf_pet_advance(&state, &zero_decay_no_halving,
                        zero_decay_no_halving.baby_duration_seconds);
        check(state.stage == KF_PET_STAGE_CHILD,
              "Baby completes into Child, the transition under test");
        const uint64_t hunger_after_baby = state.hunger_integral_mp_seconds;
        check(hunger_after_baby == 60000ull * zero_decay_no_halving.baby_duration_seconds,
              "the personality accumulator picked up exactly Baby's "
              "constant-60% contribution (60000 * baby_duration_seconds, "
              "no periodic halving with half_life == 0)");

        kf_pet_advance(&state, &zero_decay_no_halving,
                        zero_decay_no_halving.child_duration_seconds);
        check(state.stage == KF_PET_STAGE_TEEN,
              "Child completes into Teen");
        check(state.hunger_integral_mp_seconds ==
                  hunger_after_baby +
                      60000ull * zero_decay_no_halving.child_duration_seconds,
              "the accumulator carried Baby's contribution FORWARD across "
              "the Baby->Child transition and simply added Child's on top "
              "-- unlike care_integral_mp_seconds, this never reset, "
              "which is the entire point of a whole-life reading (doc 16)");
    }

    /* 4. The periodic halving itself, isolated with a small, exact
     * half-life: 1000s. A first segment of 999s (just under one
     * half-life) must NOT trigger a halving; a further 1s (landing
     * exactly on the boundary) must trigger exactly one. Zero decay again
     * holds the need constant so the expected accumulator value is exact
     * arithmetic, not an approximation. */
    {
        kf_pet_config cfg = zero_all_stage_rates(config);
        cfg.personality_recency_half_life_seconds = 1000u;
        cfg.baby_duration_seconds = 1'000'000u; /* stay in Baby throughout */

        kf_pet_state state;
        kf_pet_init(&state);
        state.stage = KF_PET_STAGE_BABY;
        state.hunger_mp = 40000u;
        state.happiness_mp = 40000u;
        state.energy_mp = 40000u;

        kf_pet_advance(&state, &cfg, 999u);
        const uint64_t before_boundary = state.hunger_integral_mp_seconds;
        check(before_boundary == 40000ull * 999u,
              "999s of a 1000s half-life accumulates at full weight, no "
              "halving triggered yet");
        check(state.care_recency_window_seconds == 999u,
              "the recency window counter tracks exactly how many seconds "
              "have accumulated toward the next halving");

        kf_pet_advance(&state, &cfg, 1u); /* the 1000th second: the boundary */
        check(state.care_recency_window_seconds == 0u,
              "landing exactly on the half-life boundary consumes the "
              "whole window and resets the remainder to 0");
        const uint64_t expected_after_boundary =
            (before_boundary >> 1) + 40000ull * 1u;
        check(state.hunger_integral_mp_seconds == expected_after_boundary,
              "crossing the half-life boundary halves the prior total "
              "BEFORE adding the new segment's own full-weight "
              "contribution -- the periodic-halving EMA doc 16 describes, "
              "not a hard reset");
        check(state.happiness_integral_mp_seconds == expected_after_boundary &&
                  state.energy_integral_mp_seconds == expected_after_boundary,
              "all three accumulators halve together on the same schedule "
              "-- they share one recency window, not three independent "
              "ones");
    }

    /* 5. kf_pet_dominant_care_trait()'s band selection: three isolated
     * pets, each with exactly one need's accumulator nonzero, must each
     * report that need's own index. Plus the documented tie-break: a
     * freshly-initialised pet (all three accumulators at their initial
     * zero, per kf_pet_init()) reports hunger (0), not an unspecified
     * value. */
    {
        kf_pet_state fresh;
        kf_pet_init(&fresh);
        check(kf_pet_dominant_care_trait(&fresh) == 0u,
              "an all-zero (freshly-initialised) pet's dominant care "
              "trait defaults to 0 (hunger) -- the documented tie-break, "
              "not an unspecified value");

        kf_pet_state hunger_led{};
        hunger_led.hunger_integral_mp_seconds = 500u;
        hunger_led.happiness_integral_mp_seconds = 100u;
        hunger_led.energy_integral_mp_seconds = 100u;
        check(kf_pet_dominant_care_trait(&hunger_led) == 0u,
              "the pet with the highest hunger accumulator reports "
              "hunger-dominant (0)");

        kf_pet_state happiness_led{};
        happiness_led.hunger_integral_mp_seconds = 100u;
        happiness_led.happiness_integral_mp_seconds = 500u;
        happiness_led.energy_integral_mp_seconds = 100u;
        check(kf_pet_dominant_care_trait(&happiness_led) == 1u,
              "the pet with the highest happiness accumulator reports "
              "happiness-dominant (1)");

        kf_pet_state energy_led{};
        energy_led.hunger_integral_mp_seconds = 100u;
        energy_led.happiness_integral_mp_seconds = 100u;
        energy_led.energy_integral_mp_seconds = 500u;
        check(kf_pet_dominant_care_trait(&energy_led) == 2u,
              "the pet with the highest energy accumulator reports "
              "energy-dominant (2)");
    }

    /* 6. The v3 save format round-trips every new field. Same wall-clock-
     * pinning technique as run_pet_stage_check()'s check 6, for the
     * identical reason: zero elapsed time on reload means nothing here
     * perturbs the very accumulators this check exists to prove
     * round-trip correctly. */
    {
        kf_pet_state state{};
        check(kf_pet_load_and_advance(&state, &config) == KF_OK,
              "load_and_advance with no prior save succeeds");

        state.base_trait = 4u;
        state.hunger_integral_mp_seconds = 111111111ull;
        state.happiness_integral_mp_seconds = 222222222ull;
        state.energy_integral_mp_seconds = 333333333ull;
        state.care_recency_window_seconds = 12345u;

        const kf_wall_time saved_at = state.last_advanced;
        check(kf_pet_save(&state) == KF_OK, "save the non-trivial state");
        check(kf_time_set_wall(saved_at.epoch_seconds) == KF_OK,
              "pin the wall clock back to exactly the saved last_advanced, "
              "guaranteeing the reload below sees zero elapsed time");

        kf_pet_state loaded{};
        check(kf_pet_load_and_advance(&loaded, &config) == KF_OK,
              "load_and_advance reloads the save");
        check(loaded.base_trait == 4u,
              "base_trait round-trips through save/load");
        check(loaded.hunger_integral_mp_seconds == 111111111ull &&
                  loaded.happiness_integral_mp_seconds == 222222222ull &&
                  loaded.energy_integral_mp_seconds == 333333333ull,
              "all three personality accumulators round-trip through "
              "save/load, byte for byte, with zero elapsed time to "
              "perturb them");
        check(loaded.care_recency_window_seconds == 12345u,
              "the recency window counter round-trips through save/load");
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

/* Proves decay is per-stage, not uniform. The specific assertion is that a
 * baby empties far faster than an adult over the SAME elapsed time -- the
 * exact rates are config and will be tuned, but "a baby is more demanding
 * than an adult" is the behaviour this whole plan exists to create, and it
 * should fail loudly if a future edit flattens the table again. */
int run_pet_demand_curve_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const kf_pet_config config = kf_pet_default_config();

    check(config.stage_rates[KF_PET_STAGE_EGG].hunger_mp_per_hour == 0u,
          "an egg does not get hungry");
    check(config.stage_rates[KF_PET_STAGE_BABY].hunger_mp_per_hour >
              config.stage_rates[KF_PET_STAGE_ADULT].hunger_mp_per_hour * 4u,
          "a baby is at least 4x as demanding as an adult");

    /* Same elapsed time, two stages, compared directly. */
    kf_pet_state baby{};
    kf_pet_init(&baby);
    baby.stage = KF_PET_STAGE_BABY;

    kf_pet_state adult{};
    kf_pet_init(&adult);
    adult.stage = KF_PET_STAGE_ADULT;

    constexpr uint32_t kOneHour = 3600u;
    apply_stage_segment_for_test(&baby, &config, kOneHour);
    apply_stage_segment_for_test(&adult, &config, kOneHour);

    check(baby.hunger_mp < adult.hunger_mp,
          "after one hour the baby is hungrier than the adult");
    check(baby.hunger_mp < 40000u,
          "after one hour a baby has lost more than half its hunger bar");
    check(adult.hunger_mp > 90000u,
          "after one hour an adult has barely moved");

    KF_LOGI(TAG, "demand curve: baby hunger %u, adult hunger %u after 1h",
            static_cast<unsigned>(baby.hunger_mp),
            static_cast<unsigned>(adult.hunger_mp));

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* The tree's shape, asserted directly. These numbers come from the character
 * bible's confirmed roster (Cut 2, Hold 3, Mark 3, Go 1) and will change as
 * the roster fills -- section 11 of the bible says Go and Cut are still
 * short. When they change, this test is the thing that should fail first,
 * which is the point of asserting them rather than trusting a constant. */
int run_evolution_tree_shape_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    check(KF_PET_TEEN_FORM_COUNT == 4u, "four verb families");
    check(kf_pet_adults_in_family(0u) == 2u, "Cut has 2 adults");
    check(kf_pet_adults_in_family(1u) == 3u, "Hold has 3 adults");
    check(kf_pet_adults_in_family(2u) == 3u, "Mark has 3 adults");
    check(kf_pet_adults_in_family(3u) == 1u, "Go has 1 adult");

    uint32_t total = 0u;
    for (uint8_t f = 0u; f < KF_PET_TEEN_FORM_COUNT; ++f) {
        total += kf_pet_adults_in_family(f);
        check(kf_pet_adults_in_family(f) <= KF_PET_ADULT_BRANCH_MAX,
              "no family exceeds KF_PET_ADULT_BRANCH_MAX");
    }
    check(total == 9u, "nine adults across the four families");

    /* Out of range must not read off the end of the table. */
    check(kf_pet_adults_in_family(KF_PET_TEEN_FORM_COUNT) == 1u,
          "an out-of-range family degrades to a single adult");

    KF_LOGI(TAG, "tree: %u families, %u adults, max %u per family",
            static_cast<unsigned>(KF_PET_TEEN_FORM_COUNT),
            static_cast<unsigned>(total),
            static_cast<unsigned>(KF_PET_ADULT_BRANCH_MAX));

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Advances `state` from a fresh egg all the way through Baby and Child and
 * into Teen, using `config`'s OWN stage durations rather than hardcoded
 * seconds -- so this stays correct however egg/baby/child_duration_seconds
 * are tuned later, the same "derive from config, don't hardcode" discipline
 * run_pet_stage_check() already follows elsewhere in this file. */
void advance_to_teen_for_test(kf_pet_state *state, const kf_pet_config *config) {
    const uint32_t total_to_teen = config->egg_duration_seconds +
                                    config->baby_duration_seconds +
                                    config->child_duration_seconds;
    kf_pet_advance(state, config, total_to_teen);
}

/* Hokorimaru is what a creature becomes when it is never touched at all --
 * character bible section 8. This is deliberately a DIFFERENT condition from
 * the care-quality average every other branch uses: a badly-cared-for
 * creature holds a grudge, an untouched one gathers dust, and the bible is
 * explicit that those are different and sadder things. */
int run_hokorimaru_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const kf_pet_config config = kf_pet_default_config();

    /* A creature nobody ever touches DIES. Chris's call, and the reason the
     * dust form is no longer reached that way: left in a drawer it runs its
     * needs down, and death arrives during childhood, days before the
     * branch point that would have made it dust. */
    kf_pet_state abandoned{};
    kf_pet_init(&abandoned);
    advance_to_teen_for_test(&abandoned, &config);
    check(abandoned.care_actions_taken == 0u, "never touched");
    check(abandoned.dead, "and dead -- a drawer is fatal, as it should be");
    check(abandoned.stage < KF_PET_STAGE_TEEN,
          "it never even reached the branch point, which is exactly why the "
          "dust form needed a different route");

    /* The route it got: kept alive and nothing more. Needs pinned low
     * enough to average under dust_care_average_mp, but above the neglect
     * threshold so the creature survives to be judged -- which is the whole
     * needle this condition has to thread. Decay is zeroed so the average
     * is exactly the level set here rather than a decay curve's integral. */
    kf_pet_config held = zero_all_stage_rates(config);
    /* Mess off too, not just decay. Poops and dirtiness are neglect
     * channels in their own right (see is_neglected()), and over a
     * three-day childhood they would saturate and kill this creature long
     * before the branch point -- which is true and correct behaviour, and
     * has nothing to do with what this check is about. The zero sentinels
     * are the documented way to say "no mess"; see kf_pet_config. */
    held.poop_interval_seconds = 0u;
    held.dirtiness_rise_mp_per_hour = 0u;
    held.dirtiness_rise_per_poop_mp_per_hour = 0u;
    const kf_pet_millipercent kBarelyRaised = 15000u; /* 15%: under 20%, over 10% */
    check(kBarelyRaised < config.dust_care_average_mp,
          "the barely-raised level really is below the dust threshold");
    check(kBarelyRaised > config.neglect_need_mp,
          "and above the neglect threshold, so this creature stays alive "
          "long enough to become anything at all");

    kf_pet_state barely{};
    kf_pet_init(&barely);
    barely.hunger_mp = kBarelyRaised;
    barely.happiness_mp = kBarelyRaised;
    barely.energy_mp = kBarelyRaised;
    kf_pet_feed(&barely, &held, 0u); /* touched -- but only just */
    barely.hunger_mp = kBarelyRaised;
    advance_to_teen_for_test(&barely, &held);
    check(!barely.dead, "a creature kept barely alive is still alive");
    check(barely.teen_form == KF_PET_TEEN_FORM_DUST,
          "and grows into the dust form -- neglect made visible, which is "
          "the character the bible describes");

    /* Raised properly: an ordinary family. */
    kf_pet_state raised{};
    kf_pet_init(&raised);
    raised.hunger_mp = 60000u;
    raised.happiness_mp = 60000u;
    raised.energy_mp = 60000u;
    advance_to_teen_for_test(&raised, &held);
    check(raised.teen_form != KF_PET_TEEN_FORM_DUST,
          "a properly raised creature avoids the dust form");
    check(raised.teen_form < KF_PET_TEEN_FORM_COUNT,
          "and lands in one of the four families");

    /* The dust form has exactly one adult -- kf_pet_adults_in_family()
     * returns 1 for out-of-range input, and KF_PET_TEEN_FORM_DUST is
     * out-of-range by construction (it equals KF_PET_TEEN_FORM_COUNT). This
     * is being relied on deliberately here, not by accident. */
    {
        kf_pet_state dust = barely;
        kf_pet_advance(&dust, &held, held.teen_duration_seconds);
        check(dust.stage == KF_PET_STAGE_ADULT,
              "the dust form completes Teen into Adult the same as any "
              "other family");
        check(dust.adult_branch == 0u,
              "the dust form's single adult lands on branch 0, relying "
              "deliberately on kf_pet_adults_in_family()'s out-of-range "
              "return of 1 for KF_PET_TEEN_FORM_DUST");
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
/* Where --dump-fb should write the framebuffer, or nullptr for "don't".
 *
 * A checksum tells you a frame CHANGED. It cannot tell you the frame is
 * WRONG, and the two questions need different tools: the golden checksums in
 * this file would happily lock in a layout with every widget drawn twice, and
 * did not notice when exactly that reached real hardware. This writes the
 * frame somewhere a human (or a vision model) can look at it. */
const char *g_dump_path = nullptr;

/* Binary PPM (P6). Chosen over PNG because it needs no library at all -- a
 * 15-line writer against a format every image viewer and converter already
 * reads is a better trade here than a dependency taken on for debugging. */
void dump_framebuffer_ppm(const char *path) {
    if (path == nullptr) {
        return;
    }
    std::FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        KF_LOGE(TAG, "could not open %s for writing", path);
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);

    const kf_color *px = kf_fb_pixels();
    for (int i = 0; i < KF_DISPLAY_WIDTH * KF_DISPLAY_HEIGHT; ++i) {
        /* RGB565 -> RGB888, replicating the high bits into the low ones so
         * white stays 0xFF rather than 0xF8. */
        const kf_color c = px[i];
        const uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1F);
        const uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3F);
        const uint8_t b5 = static_cast<uint8_t>(c & 0x1F);
        const uint8_t rgb[3] = {
            static_cast<uint8_t>((r5 << 3) | (r5 >> 2)),
            static_cast<uint8_t>((g6 << 2) | (g6 >> 4)),
            static_cast<uint8_t>((b5 << 3) | (b5 >> 2)),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    KF_LOGI(TAG, "framebuffer written to %s", path);
}

/* Poops are a COUNT, not a list. Where each one sits on screen is
 * presentation and belongs to the screen; the simulation only needs to know
 * how many are waiting. That is what makes this survive offline
 * fast-forward without storing anything per-poop. */
int run_pet_mess_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    kf_pet_config config = kf_pet_default_config();
    /* Death off. This check saturates the poop counter by advancing fifty
     * hours with no care at all, which under the real rules kills the
     * creature -- and a dead creature ignores every care action, so the
     * flush below would silently do nothing. The zero sentinel is the
     * documented way to say "cannot die"; what this check is about is where
     * poops come from and where they go. Survival has its own checks. */
    config.sickness_death_seconds = 0u;

    kf_pet_state pet{};
    kf_pet_init(&pet);
    pet.stage = KF_PET_STAGE_CHILD;   /* eggs and babies still decay-exempt */
    check(pet.poop_count == 0u, "a fresh pet has not pooped");

    /* Long enough for several intervals, in one segment. */
    apply_stage_segment_for_test(&pet, &config, config.poop_interval_seconds * 3u);
    check(pet.poop_count >= 3u, "poops accumulate over time");

    /* They pile up, but not without limit -- an unbounded count would
     * eventually overflow the screen and mean nothing extra. */
    apply_stage_segment_for_test(&pet, &config, config.poop_interval_seconds * 100u);
    check(pet.poop_count == KF_PET_MAX_POOPS, "poop count saturates rather than growing forever");

    /* Flushing clears all of them, always -- there are no degrees of it.
     * A bath deliberately does NOT: washing the creature and clearing up
     * after it are two different jobs, and only one of them is something
     * the creature has an opinion about. */
    kf_pet_bath(&pet, &config, 0u);
    check(pet.poop_count == KF_PET_MAX_POOPS,
          "a bath washes the creature and leaves the floor alone");
    kf_pet_flush(&pet);
    check(pet.poop_count == 0u, "flushing clears every poop");

    /* Feeding brings the next one sooner. */
    kf_pet_state fed{};
    kf_pet_init(&fed);
    fed.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&fed, &config, 0u);
    check(fed.seconds_until_next_poop == config.poop_interval_after_feed_seconds,
          "feeding shortens the wait for the next poop");
    check(config.poop_interval_after_feed_seconds < config.poop_interval_seconds,
          "the after-feed interval really is shorter");

    /* And it shortens it by the amount THIS config says, not the default's.
     * Every tuning figure in the care loop lives in kf_pet_config precisely
     * so it can be changed in one place; a care action that reaches past
     * its caller's config for a default would make that promise false, and
     * would do it silently -- the pet would simply poop on the wrong
     * schedule with no error anywhere. */
    kf_pet_config tuned = kf_pet_default_config();
    tuned.poop_interval_seconds = 900u;
    tuned.poop_interval_after_feed_seconds = 120u;

    kf_pet_state tuned_pet{};
    kf_pet_init(&tuned_pet);
    tuned_pet.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&tuned_pet, &tuned, 0u);
    check(tuned_pet.seconds_until_next_poop == tuned.poop_interval_after_feed_seconds,
          "feeding honours the caller's config, not the default one");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Dirtiness is the continuous half of mess, and it is deliberately NOT a
 * fourth need bar -- it has no restore action of its own, it rises faster
 * the more poops are waiting, and it shows itself as flies and stink lines
 * rather than a number. See the care-loop spec's section 4. */
int run_pet_dirtiness_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    kf_pet_config config = kf_pet_default_config();

    kf_pet_state clean_pet{};
    kf_pet_init(&clean_pet);
    clean_pet.stage = KF_PET_STAGE_CHILD;

    kf_pet_state messy{};
    kf_pet_init(&messy);
    messy.stage = KF_PET_STAGE_CHILD;

    /* Same elapsed time; the messy one starts with poops already down.
     * Poops are cleared each iteration on the clean pet so the ONLY
     * difference between them is the mess. */
    constexpr uint32_t kHour = 3600u;
    apply_stage_segment_for_test(&clean_pet, &config, kHour);
    clean_pet.poop_count = 0u;

    messy.poop_count = KF_PET_MAX_POOPS;
    apply_stage_segment_for_test(&messy, &config, kHour);

    check(clean_pet.dirtiness_mp > 0u, "a pet gets dirty just by existing");
    check(messy.dirtiness_mp > clean_pet.dirtiness_mp,
          "poops on the floor make it dirty faster");

    check(KF_PET_DIRTY_FLIES_MP < KF_PET_DIRTY_STINK_MP,
          "flies show up before stink lines");

    /* Cleaning washes the creature, not just the floor.
     *
     * Explicitly the variation this creature LIKES, because how thoroughly
     * a clean works now depends on that (see kf_pet_bath()) and this
     * creature's base trait was rolled at random. Variation 0 would make
     * this check pass or fail on the dice rather than on the behaviour it
     * is about, which is a worse kind of failure than a loud one. What a
     * disliked clean leaves behind is checked in the care-variation check,
     * where it is the actual subject. */
    uint8_t liked_clean = 0u;
    for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
        if (kf_pet_reaction_to(messy.base_trait, KF_PET_CARE_BATH, v) ==
            KF_PET_REACTION_LIKED) {
            liked_clean = v;
        }
    }
    kf_pet_bath(&messy, &config, liked_clean);
    check(messy.dirtiness_mp == 0u, "cleaning also washes the creature");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Sickness is a STATE, not a stat -- there is no sickness bar, and nothing
 * the player presses is "the medicine button". It is what sustained neglect
 * turns into, and the way out is the same care that would have prevented
 * it. See the care-loop spec's section 7.
 *
 * Every sequence below uses at least two segments to reach illness, which
 * is not padding: neglect is sampled at both ends of a segment and each end
 * counts for half, so a single long segment that starts from a healthy
 * creature nets exactly zero. One call to make things bad, then one that is
 * bad at both ends. */
int run_pet_sickness_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const kf_pet_config config = kf_pet_default_config();

    /* Cared for once, then abandoned. The feed matters: an untouched
     * creature is on the dust path and deliberately never sickens, so
     * without it nothing below happens at all. */
    kf_pet_state pet{};
    kf_pet_init(&pet);
    pet.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&pet, &config, 0u);
    check(!pet.sick, "a fresh creature is not sick");
    check(pet.neglect_seconds == 0u, "and has accumulated no neglect");

    /* Four hours from full. The needs cross the neglect threshold about
     * two and three quarter hours in, so roughly an hour and a quarter of
     * this counts as neglect -- against nearly three hours of the creature
     * being fine, which works the accumulator back down faster than the
     * tail end pushes it up. Net zero, from a starting point of zero. */
    apply_stage_segment_for_test(&pet, &config, 4u * 3600u);
    check(pet.neglect_seconds == 0u,
          "a stretch that is mostly fine does not leave the creature owing "
          "anything");

    /* Two more hours. The needs are already empty, so all of it counts. */
    apply_stage_segment_for_test(&pet, &config, 2u * 3600u);
    check(pet.neglect_seconds > 0u,
          "neglect accumulates once things are already bad");
    check(!pet.sick, "but two hours of it is not yet illness");

    /* Past the onset threshold. */
    apply_stage_segment_for_test(&pet, &config, config.sickness_onset_seconds);
    check(pet.sick, "sustained neglect makes the creature sick");

    /* One round of every button does not undo it. That is the whole point
     * of curing through care rather than through a medicine action. */
    kf_pet_feed(&pet, &config, 0u);
    kf_pet_play(&pet, &config, 0u);
    kf_pet_rest(&pet, &config, 0u);
    kf_pet_bath(&pet, &config, 0u);
    kf_pet_flush(&pet);
    check(pet.sick, "a single round of care does not cure it on the spot");

    /* Sustained care does. Sixty ten-minute stretches of being properly
     * looked after -- comfortably more than the accumulated neglect, since
     * what is being checked is that recovery HAPPENS, not the exact rate. */
    for (int i = 0; i < 60; ++i) {
        kf_pet_feed(&pet, &config, 0u);
        kf_pet_play(&pet, &config, 0u);
        kf_pet_rest(&pet, &config, 0u);
        kf_pet_bath(&pet, &config, 0u);
        kf_pet_flush(&pet);
        apply_stage_segment_for_test(&pet, &config, 600u);
    }
    check(pet.neglect_seconds == 0u, "attentive care works the clock back down");
    check(!pet.sick, "and the creature recovers");

    /* Filth alone is enough, with every need full -- mess is a real neglect
     * channel, not decoration on top of the three bars. */
    kf_pet_state filthy{};
    kf_pet_init(&filthy);
    filthy.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&filthy, &config, 0u);
    filthy.dirtiness_mp = KF_PET_MILLIPERCENT_MAX;
    filthy.poop_count = KF_PET_MAX_POOPS;
    apply_stage_segment_for_test(&filthy, &config, 600u);
    check(filthy.neglect_seconds > 0u,
          "filth counts as neglect even with the needs untouched");

    /* Never touched at all: the dust path, not the sick path. */
    kf_pet_state untouched{};
    kf_pet_init(&untouched);
    untouched.stage = KF_PET_STAGE_CHILD;
    apply_stage_segment_for_test(&untouched, &config, 4u * 3600u);
    apply_stage_segment_for_test(&untouched, &config,
                                  config.sickness_onset_seconds * 4u);
    check(untouched.care_actions_taken == 0u, "still never touched");
    check(untouched.sick,
          "a creature nobody ever touches sickens like any other -- there "
          "is no exemption for total neglect, which is the whole point of "
          "a drawer being fatal");

    /* The case this whole mechanic exists for: the device spent a day in a
     * drawer. ONE kf_pet_advance() call, one segment, a creature that was
     * full when it went in and empty when it came out.
     *
     * This is the check that matters most and the one easiest to lose. The
     * needs bottom out a few hours in, so the overwhelming majority of that
     * day was neglect -- and a creature that can be abandoned for a day
     * with no consequence has no care loop worth the name. */
    kf_pet_state drawer{};
    kf_pet_init(&drawer);
    drawer.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&drawer, &config, 0u);
    kf_pet_advance(&drawer, &config, 86400u);
    check(drawer.sick, "a day in a drawer makes a cared-for creature ill");
    check(drawer.neglect_seconds > 12u * 3600u,
          "and most of that day counts as neglect, not half of it -- the "
          "needs were gone long before the halfway mark");

    /* Illness compounds. Two creatures, same stage, same full needs, same
     * elapsed time -- the only difference between them is that one is ill.
     * Both are fed once first, because an untouched creature would be
     * exempt from the accumulator and drift out of the comparison. */
    kf_pet_state well{};
    kf_pet_init(&well);
    well.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&well, &config, 0u);

    kf_pet_state ill = well;
    ill.sick = true;

    apply_stage_segment_for_test(&well, &config, 3600u);
    apply_stage_segment_for_test(&ill, &config, 3600u);

    check(ill.hunger_mp < well.hunger_mp, "an ill creature gets hungry faster");
    check(ill.energy_mp < well.energy_mp, "and tires faster");
    check(ill.happiness_mp < well.happiness_mp,
          "and is unhappier still -- illness drains happiness on top of the "
          "faster decay, which is what makes an ignored illness spiral "
          "rather than merely tick along");
    check(config.sick_decay_multiplier_percent > 100u,
          "the multiplier really does make things worse, not better");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Death is real, and it is the end of that creature. It is also heavily
 * telegraphed: the accumulator that made it ill keeps climbing for most of
 * a day afterwards, and the screen has both thresholds to escalate
 * against. Every death should feel deserved -- the care-loop spec's
 * section 7 rejects sudden death for exactly that reason. */
int run_pet_death_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const kf_pet_config config = kf_pet_default_config();

    check(config.sickness_death_seconds > config.sickness_onset_seconds,
          "there is a real window between falling ill and dying -- a death "
          "with no warning is the one thing this must never be");

    kf_pet_state pet{};
    kf_pet_init(&pet);
    pet.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&pet, &config, 0u);

    /* Bad, then ill. Two segments, for the both-ends-sampling reason given
     * at the top of run_pet_sickness_check(). */
    apply_stage_segment_for_test(&pet, &config, 4u * 3600u);
    apply_stage_segment_for_test(&pet, &config, config.sickness_onset_seconds);
    check(pet.sick, "ill first");
    check(!pet.dead, "and still alive by a long way");

    /* Abandoned right through the window. */
    apply_stage_segment_for_test(&pet, &config, config.sickness_death_seconds);
    check(pet.dead, "sustained critical neglect is fatal");

    /* Dead is dead. Neither time nor care moves it. */
    const kf_pet_millipercent hunger_at_death = pet.hunger_mp;
    const kf_pet_stage stage_at_death = pet.stage;
    const uint32_t care_at_death = pet.care_actions_taken;
    kf_pet_advance(&pet, &config, 7u * 86400u);
    kf_pet_feed(&pet, &config, 0u);
    kf_pet_play(&pet, &config, 0u);
    kf_pet_rest(&pet, &config, 0u);
    kf_pet_bath(&pet, &config, 0u);
    kf_pet_flush(&pet);
    check(pet.dead, "a week of care does not bring it back");
    check(pet.hunger_mp == hunger_at_death, "nothing decays after death");
    check(pet.stage == stage_at_death, "and it does not keep growing up");
    check(pet.care_actions_taken == care_at_death,
          "care aimed at a dead creature does not count as care -- it must "
          "not be able to move it off the dust path posthumously");

    /* Dying partway through a single advance stops the creature there. A
     * child stage runs two days; three days of abandonment kills it inside
     * the first, and it must not go on to be handed a teen form afterwards
     * -- the branch it would receive comes from a care average covering
     * time it did not live through. */
    kf_pet_state cut_short{};
    kf_pet_init(&cut_short);
    cut_short.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&cut_short, &config, 0u);
    kf_pet_advance(&cut_short, &config, 3u * 86400u);
    check(cut_short.dead, "three days of abandonment is fatal");
    check(cut_short.stage == KF_PET_STAGE_CHILD,
          "and the creature does not evolve in the same call that killed it");

    /* The off switch: zero means it cannot die, the same sentinel shape
     * poop_interval_seconds == 0 already uses for "no mess". */
    kf_pet_config immortal = kf_pet_default_config();
    immortal.sickness_death_seconds = 0u;

    kf_pet_state survivor{};
    kf_pet_init(&survivor);
    survivor.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&survivor, &immortal, 0u);
    apply_stage_segment_for_test(&survivor, &immortal, 4u * 3600u);
    apply_stage_segment_for_test(&survivor, &immortal, 30u * 86400u);
    check(survivor.sick, "still gets ill");
    check(!survivor.dead, "but a zero death threshold means it cannot die");

    /* And the never-touched creature still walks its own path. */
    kf_pet_state untouched{};
    kf_pet_init(&untouched);
    untouched.stage = KF_PET_STAGE_CHILD;
    apply_stage_segment_for_test(&untouched, &config, 4u * 3600u);
    apply_stage_segment_for_test(&untouched, &config, 30u * 86400u);
    check(untouched.dead,
          "a creature that has never been touched dies of it, exactly as an "
          "original Tamagotchi left in a drawer does");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Investigates a real bug report: "it evolved from baby to teen, then it
 * also didn't go past teen to adult yet and I couldn't drag the timeline
 * there." Drives the actual session/seek surface a real build and the SDL
 * debug window use (kf_pet_session_frame(), kf_pet_session_debug_seek(),
 * kf_pet_session_debug_age_seconds()) -- not kf_pet_advance() on a bare
 * state the way run_pet_stage_check()/run_pet_death_check() above do --
 * because the report is about the SESSION/TIMELINE surface reaching Adult,
 * not about whether Core's stage-transition arithmetic is capable of it
 * (check 5 of run_pet_stage_check() already proves that in isolation).
 *
 * Two leading candidate explanations, both worth ruling in or out before
 * touching anything:
 *
 *   (a) the timeline/seek mechanism itself is broken -- dragging to the
 *       axis's own right edge does not actually reach the Adult boundary.
 *   (b) the reported creature was never cared for at all (Task 6, which
 *       added the only way to feed/play/rest/bathe it, did not exist yet)
 *       and default config's death-by-neglect rule (hakoniwaos/src/
 *       pet.cpp's advance_to_next_stage() CHILD-case comment: an untouched
 *       creature "now dies during CHILD, days before this branch point")
 *       killed it before Adult was ever reached in its real lived history
 *       -- in which case no amount of seeking can reach a stage transition
 *       that never happened, and that is the intended rule working exactly
 *       as designed, not a defect in the seek code.
 *
 * This proves (b) and disproves (a) by running the SAME kf_pet_session_
 * debug_seek() call twice against two pets that only differ in whether
 * they were cared for: a NEGLECTED pet (mirrors the bug report -- no care
 * calls at all, the only thing possible before Task 6) and a CARED pet
 * (fed/played/rested/bathed/flushed every flush interval, so it never
 * crosses neglect_need_mp/neglect_poop_count/neglect_dirtiness_mp). Both
 * are driven forward via kf_pet_session_frame() with KF_PET_SESSION_
 * FLUSH_SECONDS-sized steps -- genuine live-tick chunking, the same
 * granularity a real running build uses, not one giant kf_pet_advance()
 * jump -- because the size of the segments apply_stage_segment()
 * (hakoniwaos/src/pet.cpp) sees changes how much of a segment its neglect
 * estimate attributes to "already neglected" vs "just crossed the
 * threshold", which is exactly the kind of chunking-dependent
 * approximation kf_pet_session.h's own header comment already warns
 * kf_pet_session_debug_seek() inherits (there, described for branch
 * selection) -- reproducing genuine per-flush ticks here keeps this check
 * honest about what a REAL play session would have seen, not an artifact
 * of jumping in one enormous step. */
int run_pet_adult_reachability_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-adult-reachability-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_pet_session_init();

    const kf_pet_config config = kf_pet_default_config();
    /* Same sum kf_pet_session.cpp's own (private) elapsed_before_stage()
     * and sdl_debug_window.cpp's timeline_axis_max_seconds() both compute
     * -- the moment Adult begins, and the debug timeline's own right edge
     * (timeline_seconds_for_x() clamps a drag to exactly this). This file
     * has no way to reach either of those; four lines of addition is not
     * worth exporting one for, the same call sdl_debug_window.cpp's own
     * timeline_tick_seconds() comment already makes. */
    const uint64_t axis_max_seconds = config.egg_duration_seconds +
                                       config.baby_duration_seconds +
                                       config.child_duration_seconds +
                                       config.teen_duration_seconds;

    constexpr uint32_t kStepMs = KF_PET_SESSION_FLUSH_SECONDS * 1000u;

    /* Part (b): a NEGLECTED pet -- no care calls at all, mirroring the bug
     * report exactly (Task 6, the only way to care for a running build,
     * did not exist yet). Ticked forward live-tick style until it dies or
     * a generous cap is hit (enough real ticks to cover several times
     * config.sickness_death_seconds even from a standing start, so this
     * cannot loop forever if the death rule itself ever regressed to "does
     * not fire"). */
    {
        bool died = false;
        uint64_t death_age_seconds = 0u;
        kf_pet_stage death_stage = KF_PET_STAGE_EGG;
        constexpr uint32_t kMaxSteps = 200000u; /* ~69 days of ticks */
        for (uint32_t i = 0; i < kMaxSteps; ++i) {
            kf_pet_session_frame(kStepMs);
            if (kf_pet_session_state()->dead) {
                died = true;
                death_age_seconds = kf_pet_session_debug_age_seconds();
                death_stage = kf_pet_session_state()->stage;
                break;
            }
        }

        check(died, "an entirely uncared-for pet, ticked forward live-tick "
                    "style with no care calls at all, dies of sustained "
                    "neglect within a generous cap -- see hakoniwaos/src/"
                    "pet.cpp's advance_to_next_stage() CHILD-case comment");
        check(death_stage != KF_PET_STAGE_ADULT,
              "it dies before ever reaching Adult -- an uncared creature is "
              "not supposed to make it there at all");
        check(death_age_seconds < axis_max_seconds,
              "death lands before the timeline's own axis_max (the Adult "
              "boundary) on the pet's own lifetime clock, not past it");
        KF_LOGI(TAG,
                "neglected pet: died at age %llu s (stage %d), axis_max %llu s",
                static_cast<unsigned long long>(death_age_seconds),
                static_cast<int>(death_stage),
                static_cast<unsigned long long>(axis_max_seconds));

        /* The exact move from the bug report: drag the timeline as far
         * right as it goes. sdl_debug_window.cpp's timeline_seconds_for_x()
         * clamps any drag to [0, axis_max_seconds], so this IS "as far
         * right as the timeline lets you drag", not an unrealistic
         * out-of-range probe. */
        kf_pet_session_debug_seek(axis_max_seconds);

        check(kf_pet_session_state()->dead,
              "still dead after seeking to axis_max -- dead is terminal, "
              "seeking cannot revive a creature (kf_pet_advance()'s own "
              "leading `if (state->dead) return;`)");
        check(kf_pet_session_state()->stage == death_stage,
              "seeking all the way to axis_max does not move a dead "
              "creature's stage at all -- it lands exactly where it died, "
              "not at Adult, however far right the drag goes");
        check(kf_pet_session_debug_age_seconds() == death_age_seconds,
              "a dead pet's own lifetime clock is frozen -- seeking forward "
              "past death does not re-run or advance it");
    }

    /* Part (a): the SAME kf_pet_session_debug_seek() call, against a pet
     * that only differs in having been cared for -- fed/played/rested/
     * bathed/flushed every single flush interval, so hunger/happiness/
     * energy never approach neglect_need_mp (each care call restores at
     * LEAST care_boost_disliked_mp, 10%, against at most a few tenths of a
     * percent of decay per 30-second step) and poop_count/dirtiness_mp
     * never approach their own neglect thresholds either. If this pet
     * reaches Adult via the identical seek() call the neglected pet above
     * just failed to reach, that disproves candidate (a): the timeline/
     * seek mechanism itself is not broken, only unable to produce a stage
     * transition a dead creature's own history never lived through. */
    {
        kf_pet_session_debug_reset(); /* fresh egg; also clears the ring
                                        * the neglected pet's history left
                                        * in it (kf_pet_session.h's own
                                        * kf_pet_session_debug_reset()
                                        * comment) -- this pet gets a
                                        * genuinely clean timeline. */

        const uint32_t total_steps =
            static_cast<uint32_t>(axis_max_seconds * 1000ull / kStepMs) + 2u;
        for (uint32_t i = 0; i < total_steps; ++i) {
            kf_pet_session_frame(kStepMs);
            /* Variation is irrelevant here -- this is about keeping every
             * need and mess signal away from its own neglect threshold,
             * not about which reaction lands, so 0u throughout is fine. */
            kf_pet_session_feed(0u);
            kf_pet_session_play(0u);
            kf_pet_session_rest(0u);
            kf_pet_session_bath(0u);
            kf_pet_session_flush();
            if (kf_pet_session_state()->stage == KF_PET_STAGE_ADULT) {
                break;
            }
        }

        check(!kf_pet_session_state()->dead,
              "a genuinely cared-for pet, ticked forward the same live-tick "
              "way, never dies");
        check(kf_pet_session_state()->stage == KF_PET_STAGE_ADULT,
              "and reaches Adult on its own, live-ticked, well before this "
              "generous step cap -- proving Adult is reachable through the "
              "real session surface at all, the same one the neglected pet "
              "above used");

        /* Rewind, then reproduce the bug report's exact drag once more --
         * this time against a pet whose own history genuinely does include
         * an Adult transition.
         *
         * Not asserting the rewind lands on EXACTLY age 0/Egg: the live-
         * tick loop above pushed a snapshot on every 30-second frame flush
         * PLUS every one of the five care calls each iteration -- six
         * pushes per iteration, ~17000 iterations, tens of thousands of
         * pushes against a 2048-entry ring (kDebugSnapshotCapacity,
         * kf_pet_session.cpp) -- so genesis has long since been evicted by
         * the time this runs, and kf_pet_session_debug_seek()'s own
         * documented clamping (kf_pet_session.h: "seeking earlier than the
         * oldest surviving snapshot clamps to that snapshot instead of
         * erroring") is expected and correct here, not a bug to route
         * around. What this DOES prove: seeking away from Adult actually
         * moves a live pet's state at all -- the opposite of the dead
         * pet's seek above, which left stage/age completely unchanged no
         * matter the target. */
        kf_pet_session_debug_seek(0u);
        check(kf_pet_session_state()->stage != KF_PET_STAGE_ADULT,
              "seeking toward age 0 actually moves a live pet away from "
              "Adult (unlike the dead pet above, whose seek left it frozen "
              "exactly where it died no matter the target) -- confirms "
              "this is not a seek that silently does nothing for every "
              "pet");

        kf_pet_session_debug_seek(axis_max_seconds);
        check(kf_pet_session_state()->stage == KF_PET_STAGE_ADULT,
              "dragging all the way right reaches Adult for a pet whose "
              "real history includes it -- the timeline/seek mechanism "
              "(kf_pet_session_debug_seek(), timeline_seconds_for_x()'s "
              "[0, axis_max] clamp) works correctly; the neglected pet "
              "above could not reach Adult because it never lived to see "
              "one, not because seeking is broken");
    }

    kf_pet_session_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the debug stage-jump (Chris's own request: a button that "spawns
 * the creature at full care stats... at each button press and timeline
 * moves to the beginning of each life stage", so looking at a stage's art
 * and care behaviour does not require nursing a pet through every real
 * stage first) lands exactly where it claims to, every time it is asked:
 * the requested stage, alive, not sick, needs full, at the very start of
 * that stage -- that an explicit teen_form/adult_branch index is honoured
 * exactly wherever the stage has reached that branch point, defaults to 0
 * both when unset AND when out of range (see kf_pet_session_debug_jump_
 * to_stage()'s own header comment on why those are the same safe case),
 * and stays at 0 when the stage has NOT reached that branch point yet even
 * if a nonzero index was requested. Also proves the actual point of the
 * feature: a genuinely dead, sick, neglected pet (killed by a real
 * kf_pet_advance() run, not a hand-set flag) comes back alive and clean
 * after a jump, without the "a drawer is fatal" rule having been touched
 * -- see kf/pet.h's kf_pet_advance() and its leading `if (state->dead)
 * return;`, still fully in force; this jump routes AROUND it by replacing
 * the whole state, not through it. */
int run_pet_debug_jump_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-pet-debug-jump-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_pet_session_init();

    const kf_pet_config config = kf_pet_default_config();

    /* Same sum run_pet_adult_reachability_check() above and sdl_debug_
     * window.cpp's timeline_tick_seconds() both compute independently --
     * where a given stage begins on the pet's own lifetime clock. This
     * file has no way to reach kf_pet_session.cpp's own private copy of
     * this; four lines of addition is not worth exporting one for, the
     * same call made at every other site that needs it. */
    auto elapsed_before = [&config](kf_pet_stage stage) -> uint64_t {
        uint64_t t = 0u;
        if (stage > KF_PET_STAGE_EGG) {
            t += config.egg_duration_seconds;
        }
        if (stage > KF_PET_STAGE_BABY) {
            t += config.baby_duration_seconds;
        }
        if (stage > KF_PET_STAGE_CHILD) {
            t += config.child_duration_seconds;
        }
        if (stage > KF_PET_STAGE_TEEN) {
            t += config.teen_duration_seconds;
        }
        return t;
    };

    /* 1. Every stage: lands on it, alive, not sick, needs full, mess- and
     * neglect-free, at the very start (stage_elapsed_seconds == 0 and the
     * pet's own lifetime clock reading exactly where the stage begins) --
     * requirement 1, checked for all five stages in one pass. */
    {
        constexpr kf_pet_stage kStages[] = {
            KF_PET_STAGE_EGG, KF_PET_STAGE_BABY, KF_PET_STAGE_CHILD,
            KF_PET_STAGE_TEEN, KF_PET_STAGE_ADULT};
        for (kf_pet_stage stage : kStages) {
            kf_pet_session_debug_jump_to_stage(stage, 0u, 0u);
            const kf_pet_state *s = kf_pet_session_state();
            check(s->stage == stage,
                  "a jump lands on exactly the requested stage");
            check(!s->dead, "a jumped-to pet is alive");
            check(!s->sick, "a jumped-to pet is not sick");
            check(s->hunger_mp == KF_PET_MILLIPERCENT_MAX &&
                      s->happiness_mp == KF_PET_MILLIPERCENT_MAX &&
                      s->energy_mp == KF_PET_MILLIPERCENT_MAX,
                  "a jumped-to pet has every need topped up to full");
            check(s->stage_elapsed_seconds == 0u,
                  "a jumped-to pet sits at the very start of the stage, not "
                  "partway through it");
            check(s->neglect_seconds == 0u && s->poop_count == 0u,
                  "a jumped-to pet has no accumulated neglect or mess");
            check(kf_pet_session_debug_age_seconds() == elapsed_before(stage),
                  "the pet's own lifetime clock reads exactly where this "
                  "stage begins on the timeline");
        }
    }

    /* 2. Explicit teen_form/adult_branch is honoured exactly -- every valid
     * adult index in the WIDEST family (Hold, family 1, 3 adults per
     * kAdultsInFamily in hakoniwaos/src/pet.cpp), so this is not merely a
     * coincidence of the first index happening to already be 0, plus the
     * single valid index of the NARROWEST family (Go, family 3, 1 adult). */
    {
        for (uint8_t adult = 0u; adult < 3u; ++adult) {
            kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_ADULT, 1u, adult);
            const kf_pet_state *s = kf_pet_session_state();
            check(s->teen_form == 1u, "the requested teen_form is honoured");
            check(s->adult_branch == adult,
                  "the requested adult_branch is honoured for every valid "
                  "index in a 3-adult family");
        }

        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_ADULT, 3u, 0u);
        const kf_pet_state *s = kf_pet_session_state();
        check(s->teen_form == 3u && s->adult_branch == 0u,
              "a single-adult family's only valid index (0) is honoured "
              "too, not just families with room to spare");
    }

    /* 3. Out-of-range indices clamp to 0 rather than being written raw --
     * kf_pet_adults_in_family() and every other reader of adult_branch
     * assumes it is < kf_pet_adults_in_family(teen_form), the same
     * invariant Core's own save-format unpack() enforces on load. */
    {
        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_TEEN, 200u, 0u);
        check(kf_pet_session_state()->teen_form == 0u,
              "an out-of-range teen_form clamps to 0, not written raw");

        /* Go (family 3) has only 1 adult (its only valid index is 0) --
         * adult_branch 2 is out of range for THIS family specifically, even
         * though 2 is a perfectly valid index for Hold or Mark above. */
        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_ADULT, 3u, 2u);
        check(kf_pet_session_state()->adult_branch == 0u,
              "an adult_branch out of range for the SELECTED family (not "
              "just out of range in the absolute sense) clamps to 0");
    }

    /* 4. teen_form/adult_branch stay at their "not decided yet" default
     * when the target stage has not reached their own branch point, even
     * if the caller asks for a nonzero index -- kf/pet.h's own words:
     * meaningless before the branch point, and this must not pretend
     * otherwise just because a debug caller supplied a number. */
    {
        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_BABY, 2u, 1u);
        const kf_pet_state *s = kf_pet_session_state();
        check(s->teen_form == 0u && s->adult_branch == 0u,
              "teen_form/adult_branch stay at 0 when jumping to a stage "
              "before Child->Teen has even happened, despite nonzero "
              "indices being requested");

        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_TEEN, 2u, 1u);
        check(kf_pet_session_state()->teen_form == 2u &&
                  kf_pet_session_state()->adult_branch == 0u,
              "at Teen, teen_form IS honoured (its branch point already "
              "passed) but adult_branch stays 0 -- Teen->Adult has not "
              "happened yet, even with a nonzero index requested");
    }

    /* 5. The actual point of the feature: a pet killed by a REAL kf_pet_
     * advance() run (live-ticked to death, the same technique run_pet_
     * adult_reachability_check() above uses, not a hand-set `dead` flag)
     * comes back alive, well, and clean after a jump -- without weakening
     * kf_pet_advance()'s own death-by-neglect check at all, which is still
     * exactly why a dead pet cannot simply be advanced back to health. */
    {
        kf_pet_session_debug_reset();
        constexpr uint32_t kStepMs = KF_PET_SESSION_FLUSH_SECONDS * 1000u;
        constexpr uint32_t kMaxSteps = 200000u; /* ~69 days of ticks */
        for (uint32_t i = 0; i < kMaxSteps && !kf_pet_session_state()->dead;
             ++i) {
            kf_pet_session_frame(kStepMs);
        }
        check(kf_pet_session_state()->dead,
              "the pet actually died of neglect before this check "
              "continues -- otherwise the assertions below would prove "
              "nothing about reviving a genuinely dead pet");

        kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_CHILD, 0u, 0u);
        const kf_pet_state *s = kf_pet_session_state();
        check(!s->dead, "a jump revives a dead pet into a fabricated, "
                        "living one -- the entire reason this feature "
                        "exists (kf_pet_advance() itself never revives "
                        "anything, by design)");
        check(!s->sick && s->neglect_seconds == 0u,
              "a jump also clears sickness and accumulated neglect, not "
              "just the dead flag on its own");
        check(s->stage == KF_PET_STAGE_CHILD,
              "and lands on exactly the requested stage, the same as it "
              "would for a pet that was never neglected at all");
    }

    kf_pet_session_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* The preference table is the one system in this care loop that a player
 * is meant to LEARN rather than read. That only works if it is consistent,
 * so this checks its shape exhaustively -- all six traits, all four
 * actions, all three variations -- rather than spot-checking a few. Seventy
 * two combinations is nothing to iterate and it is the difference between
 * "the table is probably fine" and "the table is fine". */
int run_pet_preferences_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    for (uint8_t trait = 0u; trait < KF_PET_BASE_TRAIT_COUNT; ++trait) {
        for (uint8_t a = 0u; a < KF_PET_CARE_ACTION_COUNT; ++a) {
            const kf_pet_care_action action = static_cast<kf_pet_care_action>(a);
            unsigned liked = 0u, neutral = 0u, disliked = 0u;
            for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
                switch (kf_pet_reaction_to(trait, action, v)) {
                case KF_PET_REACTION_LIKED:
                    liked++;
                    break;
                case KF_PET_REACTION_NEUTRAL:
                    neutral++;
                    break;
                case KF_PET_REACTION_DISLIKED:
                    disliked++;
                    break;
                default:
                    check(false, "every reaction is one of the three");
                    break;
                }
            }
            check(liked == 1u && neutral == 1u && disliked == 1u,
                  "every trait has exactly one liked, one neutral and one "
                  "disliked variation of every action -- a trait with two "
                  "favourites or none is not something a player can learn");
        }
    }

    /* Traits must actually differ, or there is nothing to discover. */
    bool any_difference = false;
    for (uint8_t a = 0u; a < KF_PET_CARE_ACTION_COUNT; ++a) {
        const kf_pet_care_action action = static_cast<kf_pet_care_action>(a);
        for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
            const uint8_t first = kf_pet_reaction_to(0u, action, v);
            for (uint8_t trait = 1u; trait < KF_PET_BASE_TRAIT_COUNT; ++trait) {
                if (kf_pet_reaction_to(trait, action, v) != first) {
                    any_difference = true;
                }
            }
        }
    }
    check(any_difference,
          "different traits want different things -- a table where every "
          "creature agrees is a table with no game in it");

    /* No two traits may be identical across the whole table, or two of the
     * six are indistinguishable in play and one of them is dead weight. */
    for (uint8_t x = 0u; x < KF_PET_BASE_TRAIT_COUNT; ++x) {
        for (uint8_t y = static_cast<uint8_t>(x + 1u);
             y < KF_PET_BASE_TRAIT_COUNT; ++y) {
            bool identical = true;
            for (uint8_t a = 0u; a < KF_PET_CARE_ACTION_COUNT; ++a) {
                const kf_pet_care_action action =
                    static_cast<kf_pet_care_action>(a);
                for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
                    if (kf_pet_reaction_to(x, action, v) !=
                        kf_pet_reaction_to(y, action, v)) {
                        identical = false;
                    }
                }
            }
            check(!identical,
                  "no two traits have identical preferences -- two traits a "
                  "player cannot tell apart are one trait and a waste of a "
                  "slot");
        }
    }

    /* Nonsense input lands somewhere defined rather than reading off the
     * end of the table. A corrupted save is the realistic route in. */
    check(kf_pet_reaction_to(200u, KF_PET_CARE_FEED, 0u) <=
              KF_PET_REACTION_DISLIKED,
          "an out-of-range trait returns a valid reaction");
    check(kf_pet_reaction_to(0u, KF_PET_CARE_FEED, 200u) <=
              KF_PET_REACTION_DISLIKED,
          "an out-of-range variation returns a valid reaction");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* What a variation is worth depends on who you gave it to. This is the
 * whole of the discovery mechanic in one function: the same button, on the
 * same creature, doing noticeably different amounts of good depending on a
 * fact the player has to work out. */
int run_pet_care_variation_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    const kf_pet_config config = kf_pet_default_config();

    check(config.care_boost_liked_mp > config.care_boost_neutral_mp &&
              config.care_boost_neutral_mp > config.care_boost_disliked_mp,
          "liked beats neutral beats disliked, or there is nothing to "
          "discover");

    /* Walk every trait rather than trusting whichever one the RNG rolled:
     * the point is that the RIGHT variation for THIS creature is worth
     * more, and that is a different variation for each trait. */
    for (uint8_t trait = 0u; trait < KF_PET_BASE_TRAIT_COUNT; ++trait) {
        uint8_t liked = 0u, disliked = 0u;
        for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
            if (kf_pet_reaction_to(trait, KF_PET_CARE_FEED, v) ==
                KF_PET_REACTION_LIKED) {
                liked = v;
            }
            if (kf_pet_reaction_to(trait, KF_PET_CARE_FEED, v) ==
                KF_PET_REACTION_DISLIKED) {
                disliked = v;
            }
        }

        kf_pet_state loved{};
        kf_pet_init(&loved);
        loved.base_trait = trait;
        loved.stage = KF_PET_STAGE_CHILD;
        loved.hunger_mp = 0u;

        kf_pet_state tolerated = loved;

        kf_pet_feed(&loved, &config, liked);
        kf_pet_feed(&tolerated, &config, disliked);

        check(loved.hunger_mp > tolerated.hunger_mp,
              "the variation this creature likes feeds it better than the "
              "one it does not");
        check(loved.last_reaction == KF_PET_REACTION_LIKED,
              "and it says so -- the reaction is what the player reads");
        check(tolerated.last_reaction == KF_PET_REACTION_DISLIKED,
              "as does the objection");
        check(loved.last_care_action == KF_PET_CARE_FEED,
              "the reaction records which action it was a reaction to, so "
              "a screen showing it knows what to draw");
    }

    /* All four actions carry it, not just feeding. */
    kf_pet_state pet{};
    kf_pet_init(&pet);
    pet.stage = KF_PET_STAGE_CHILD;
    pet.happiness_mp = 0u;
    pet.energy_mp = 0u;
    pet.poop_count = 3u;

    kf_pet_play(&pet, &config, 0u);
    check(pet.last_care_action == KF_PET_CARE_PLAY, "playing records itself");
    kf_pet_rest(&pet, &config, 0u);
    check(pet.last_care_action == KF_PET_CARE_REST, "resting records itself");
    kf_pet_bath(&pet, &config, 0u);
    check(pet.last_care_action == KF_PET_CARE_BATH, "bathing records itself");

    /* Flushing is the one mess action with no opinion attached, so it must
     * not overwrite the creature's response to the last thing actually
     * done to it -- that response is what the screen is showing. */
    const uint8_t reaction_before_flush = pet.last_reaction;
    kf_pet_flush(&pet);
    check(pet.poop_count == 0u, "flushing does its job");
    check(pet.last_care_action == KF_PET_CARE_BATH &&
              pet.last_reaction == reaction_before_flush,
          "and leaves the last reaction alone -- a chore is not a response");

    /* Being clean is a NEED, so every variation gets the creature equally
     * clean. What preference buys is a little happiness on top. Walk every
     * trait, since which variation is which differs by trait. */
    for (uint8_t trait = 0u; trait < KF_PET_BASE_TRAIT_COUNT; ++trait) {
        uint8_t liked = 0u, neutral = 0u, disliked = 0u;
        for (uint8_t v = 0u; v < KF_PET_CARE_VARIATION_COUNT; ++v) {
            switch (kf_pet_reaction_to(trait, KF_PET_CARE_BATH, v)) {
            case KF_PET_REACTION_LIKED:
                liked = v;
                break;
            case KF_PET_REACTION_DISLIKED:
                disliked = v;
                break;
            default:
                neutral = v;
                break;
            }
        }

        kf_pet_state filthy{};
        kf_pet_init(&filthy);
        filthy.base_trait = trait;
        filthy.stage = KF_PET_STAGE_CHILD;
        filthy.dirtiness_mp = KF_PET_MILLIPERCENT_MAX;
        filthy.poop_count = KF_PET_MAX_POOPS;
        filthy.happiness_mp = 0u;

        kf_pet_state tolerated = filthy;
        kf_pet_state hated = filthy;

        kf_pet_bath(&filthy, &config, liked);
        kf_pet_bath(&tolerated, &config, neutral);
        kf_pet_bath(&hated, &config, disliked);

        check(filthy.dirtiness_mp == 0u && tolerated.dirtiness_mp == 0u &&
                  hated.dirtiness_mp == 0u,
              "every variation gets the creature equally clean -- being "
              "washed is a need, and a creature left dirty because it "
              "disliked the flannel would punish the player for meeting it");
        check(filthy.poop_count == KF_PET_MAX_POOPS &&
                  hated.poop_count == KF_PET_MAX_POOPS,
              "and none of them touch the floor, whatever the creature "
              "thought of the bath");

        check(filthy.happiness_mp > tolerated.happiness_mp,
              "the way it likes to be washed cheers it up most");
        check(tolerated.happiness_mp > hated.happiness_mp,
              "a way it merely tolerates, barely");
        check(hated.happiness_mp == 0u,
              "and a way it hates does nothing at all -- nothing NEGATIVE "
              "either, since the creature still ended up clean");
    }

    /* Nonsense variation is treated as neutral rather than crashing or
     * silently becoming a favourite. */
    kf_pet_state odd{};
    kf_pet_init(&odd);
    odd.stage = KF_PET_STAGE_CHILD;
    odd.hunger_mp = 0u;
    kf_pet_feed(&odd, &config, 200u);
    check(odd.last_reaction == KF_PET_REACTION_NEUTRAL,
          "an out-of-range variation is taken as neutral");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves kf_creature_pose_for()'s precedence -- dead beats sick beats a held
 * reaction beats neutral -- and proves the reaction window actually gates
 * the sticky last_reaction field: the same LIKED reaction reads as happy
 * while the window is open and neutral once it has lapsed. See
 * kf/creature.h and kf/creature.cpp. */
int run_creature_pose_check(void) {
    kf_pet_state pet{};
    kf_pet_init(&pet);

    struct Case {
        const char *name;
        bool sick;
        bool dead;
        uint8_t reaction;
        uint32_t hold_ms;
        kf_creature_pose expect;
    };

    const Case cases[] = {
        {"fresh pet is neutral", false, false, KF_PET_REACTION_NEUTRAL, 0,
         KF_CREATURE_POSE_NEUTRAL},
        {"liked care inside the window is happy", false, false,
         KF_PET_REACTION_LIKED, 500, KF_CREATURE_POSE_HAPPY},
        {"liked care after the window lapses is neutral", false, false,
         KF_PET_REACTION_LIKED, 0, KF_CREATURE_POSE_NEUTRAL},
        {"disliked care inside the window is objecting", false, false,
         KF_PET_REACTION_DISLIKED, 500, KF_CREATURE_POSE_OBJECTING},
        {"sickness outranks a happy reaction", true, false,
         KF_PET_REACTION_LIKED, 500, KF_CREATURE_POSE_SICK},
        {"death outranks sickness", true, true, KF_PET_REACTION_LIKED, 500,
         KF_CREATURE_POSE_DEAD},
    };

    bool ok = true;
    for (const Case &c : cases) {
        pet.sick = c.sick;
        pet.dead = c.dead;
        pet.last_reaction = c.reaction;
        const kf_creature_pose got = kf_creature_pose_for(&pet, c.hold_ms);
        if (got != c.expect) {
            KF_LOGE(TAG, "FAILED: %s: expected %d, got %d", c.name,
                    static_cast<int>(c.expect), static_cast<int>(got));
            ok = false;
        }
    }

    /* Task 2 (and, for the name shape itself, Task 5): (pet, pose, dir) ->
     * the asset-pack ENTRY name, per tools/character_manifest.toml's naming
     * convention now that teen and adult sprites are branch-specific rather
     * than shared: <stage><indices>_<pose>_<dir>, no frame number -- an
     * entry holds every frame of the animation contiguously (kf/assets.h's
     * KF_ASSET_TYPE_SPRITE_INDEXED), so there is no "frame one" to name.
     * Egg collapses every pose to egg_idle_<dir> (the manifest gives the egg
     * exactly one state, "idle") -- the case that matters most, because
     * getting it wrong means the egg silently draws nothing. teen_form/
     * adult_branch come from the pet, not the stage, which is why this
     * takes a pet state rather than a bare stage now: from the teen stage
     * onward, "which sprite" cannot be answered from the stage alone. */
    struct NameCase {
        kf_pet_stage stage;
        uint8_t teen_form;
        uint8_t adult_branch;
        kf_creature_pose pose;
        kf_creature_direction dir;
        const char *expect;
    };
    const NameCase names[] = {
        /* Egg: collapses every pose to "idle", but not every direction --
         * exercises S, E and N across the collapse in one pass. */
        {KF_PET_STAGE_EGG, 0, 0, KF_CREATURE_POSE_NEUTRAL, KF_CREATURE_DIR_S,
         "egg_idle_s"},
        {KF_PET_STAGE_EGG, 0, 0, KF_CREATURE_POSE_SICK, KF_CREATURE_DIR_E,
         "egg_idle_e"},
        {KF_PET_STAGE_EGG, 0, 0, KF_CREATURE_POSE_HAPPY, KF_CREATURE_DIR_N,
         "egg_idle_n"},
        /* Baby and child: shared single designs, no branch indices. */
        {KF_PET_STAGE_BABY, 0, 0, KF_CREATURE_POSE_NEUTRAL, KF_CREATURE_DIR_S,
         "baby_neutral_s"},
        {KF_PET_STAGE_BABY, 0, 0, KF_CREATURE_POSE_SLEEPING, KF_CREATURE_DIR_E,
         "baby_sleeping_e"},
        {KF_PET_STAGE_CHILD, 0, 0, KF_CREATURE_POSE_HAPPY, KF_CREATURE_DIR_N,
         "child_happy_n"},
        /* Teen: branches by teen_form alone. form 0 and a non-zero form
         * (3), plus the dust form (4, KF_PET_TEEN_FORM_DUST). */
        {KF_PET_STAGE_TEEN, 0, 0, KF_CREATURE_POSE_OBJECTING, KF_CREATURE_DIR_S,
         "teen0_objecting_s"},
        {KF_PET_STAGE_TEEN, 3, 0, KF_CREATURE_POSE_NEUTRAL, KF_CREATURE_DIR_E,
         "teen3_neutral_e"},
        {KF_PET_STAGE_TEEN, 4, 0, KF_CREATURE_POSE_SICK, KF_CREATURE_DIR_N,
         "teen4_sick_n"},
        /* Adult: branches by teen_form AND adult_branch. Non-zero of both,
         * checked against the length worked out by hand in the task brief. */
        {KF_PET_STAGE_ADULT, 0, 0, KF_CREATURE_POSE_HAPPY, KF_CREATURE_DIR_S,
         "adult00_happy_s"},
        {KF_PET_STAGE_ADULT, 2, 1, KF_CREATURE_POSE_OBJECTING, KF_CREATURE_DIR_E,
         "adult21_objecting_e"},
        /* DEAD has no art yet and falls back to the sick sprite -- see
         * kf/creature.h. Not a defect; this is the one place that fallback
         * is pinned down by a test. */
        {KF_PET_STAGE_CHILD, 0, 0, KF_CREATURE_POSE_DEAD, KF_CREATURE_DIR_S,
         "child_sick_s"},
    };
    for (const NameCase &c : names) {
        kf_pet_state named_pet{};
        kf_pet_init(&named_pet);
        named_pet.stage = c.stage;
        named_pet.teen_form = c.teen_form;
        named_pet.adult_branch = c.adult_branch;
        char buf[32] = {0};
        kf_creature_sprite_name(&named_pet, c.pose, c.dir, buf, sizeof(buf));
        if (std::strcmp(buf, c.expect) != 0) {
            KF_LOGE(TAG, "name: expected '%s', got '%s'", c.expect, buf);
            ok = false;
        }
    }

    /* The manifest's own stated limit (tools/kf_character_manifest.py's
     * PACK_NAME_MAX_CHARS) is 31 characters. adult<teen_form><adult_branch>
     * is the longest stage token and "objecting" the longest pose, so sweep
     * every branch index and every pose/direction combination on the adult
     * stage and assert the limit directly, rather than trusting the by-hand
     * arithmetic in the task brief not to have an off-by-one in it. */
    for (uint8_t teen_form = 0; teen_form <= KF_PET_TEEN_FORM_DUST; ++teen_form) {
        for (uint8_t branch = 0; branch < KF_PET_ADULT_BRANCH_MAX; ++branch) {
            for (int pose = 0; pose < KF_CREATURE_POSE_COUNT; ++pose) {
                for (int dir = 0; dir < KF_CREATURE_DIR_COUNT; ++dir) {
                    kf_pet_state limit_pet{};
                    kf_pet_init(&limit_pet);
                    limit_pet.stage = KF_PET_STAGE_ADULT;
                    limit_pet.teen_form = teen_form;
                    limit_pet.adult_branch = branch;
                    char buf[32] = {0};
                    kf_creature_sprite_name(&limit_pet,
                                             static_cast<kf_creature_pose>(pose),
                                             static_cast<kf_creature_direction>(dir),
                                             buf, sizeof(buf));
                    if (std::strlen(buf) > 31u) {
                        KF_LOGE(TAG,
                                "name: '%s' is %zu chars, over the 31-char "
                                "manifest limit",
                                buf, std::strlen(buf));
                        ok = false;
                    }
                }
            }
        }
    }

    if (ok) {
        KF_LOGI(TAG, "all %zu cases passed", sizeof(cases) / sizeof(cases[0]));
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the wander itself (Task 3): the same seed produces the same walk
 * twice, the creature never leaves the field across many seeds and a long
 * run, and it actually moves rather than trivially passing the first two
 * checks by standing still forever. See kf/creature.h's kf_creature_init()/
 * kf_creature_update()/kf_creature_bounds(). */
static int run_creature_wander_check(void) {
    const kf_rect field = {0, 0, 240, 200};
    int failures = 0;

    /* Same seed, same walk -- twice. */
    kf_creature a;
    kf_creature b;
    kf_rng_seed(12345u);
    kf_creature_init(&a, field);
    for (int i = 0; i < 600; ++i) { kf_creature_update(&a, field, 33u); }
    kf_rng_seed(12345u);
    kf_creature_init(&b, field);
    for (int i = 0; i < 600; ++i) { kf_creature_update(&b, field, 33u); }
    /* dir is compared too, not just x/y: it is written from
     * direction_for_delta() (hakoniwaos/src/creature.cpp), a pure function
     * of state already covered by the position check above, so any
     * divergence here would mean dir itself depends on something outside
     * that -- uninitialised memory, most plausibly -- that this test would
     * otherwise never catch. */
    if (a.x != b.x || a.y != b.y || a.dir != b.dir) {
        KF_LOGE(TAG,
                "creature-wander: same seed diverged: (%d,%d,dir=%d) vs "
                "(%d,%d,dir=%d)",
                a.x, a.y, static_cast<int>(a.dir), b.x, b.y,
                static_cast<int>(b.dir));
        ++failures;
    }

    /* It stays on the field, for a long time, from many seeds. */
    for (uint32_t seed = 1u; seed <= 50u; ++seed) {
        kf_creature c;
        kf_rng_seed(seed);
        kf_creature_init(&c, field);
        for (int i = 0; i < 2000; ++i) {
            kf_creature_update(&c, field, 33u);
            const kf_rect r = kf_creature_bounds(&c);
            if (r.x0 < field.x0 || r.y0 < field.y0 || r.x1 > field.x1 ||
                r.y1 > field.y1) {
                KF_LOGE(TAG,
                        "creature-wander: seed %u escaped the field at step "
                        "%d: (%d,%d,%d,%d)",
                        seed, i, r.x0, r.y0, r.x1, r.y1);
                ++failures;
                break;
            }
        }
    }

    /* It actually moves -- a creature that never walks would pass the two
     * checks above trivially. */
    kf_creature d;
    kf_rng_seed(7u);
    kf_creature_init(&d, field);
    const int32_t start_x = d.x;
    bool moved = false;
    for (int i = 0; i < 2000 && !moved; ++i) {
        kf_creature_update(&d, field, 33u);
        if (d.x != start_x) { moved = true; }
    }
    if (!moved) {
        KF_LOGE(TAG, "creature-wander: never moved in 2000 steps");
        ++failures;
    }

    if (failures == 0) {
        KF_LOGI(TAG,
                "creature-wander: determinism, bounds and movement all "
                "hold");
    }
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}

/* Proves the pet screen's ownership switch (Task 4) stays inside its
 * dirty-rect budget: at most 2 rectangles a frame (erase the creature's
 * previous position, draw its new one -- see kf_creature_screen.h) and at
 * most three 48x48 sprite areas' worth of bytes, generous headroom over
 * the two areas an erase-then-draw frame actually touches. Also proves the
 * budget is not being met VACUOUSLY, by drawing nothing: the default asset
 * pack has no creature art (examples/hello_sprite/assets.kfpack carries
 * exactly one sprite, "test_sprite"), so every kf_assets_get() lookup
 * kf_creature_screen_frame() makes returns null and it falls back to a
 * placeholder rectangle (kf_creature_screen.cpp's kPlaceholderColor) --
 * this asserts that placeholder rectangle's PIXELS really did change to
 * something other than the background colour, not just that some
 * rectangle got marked dirty (kf_fill_rect(g_previous, kBackground) does
 * that unconditionally every frame, draw call or not, which is what makes
 * a rect-count-only version of this check pass vacuously -- see
 * creature_pixel_ever_drawn's own comment below for how this one avoids
 * that trap).
 *
 * Bypasses kf_app_frame()/kf_screen_nav.cpp entirely -- this is a
 * rendering-cost check on kf_creature_screen_frame() itself, not on screen
 * switching (see run_screen_nav_check() for that one). Same isolated-per-
 * PID storage directory trick as run_pet_screen_check() below, for the
 * same reason: kf_pet_session_init() needs somewhere to read/write that
 * will not collide with another test running at the same time. Mounts the
 * real default asset pack (kf_assets_init()), rather than leaving assets
 * uninitialised, so this genuinely exercises the "no creature art in the
 * pack" scenario the fallback exists for, not just an equivalent-looking
 * uninitialised table.
 *
 * Forces the pet off KF_PET_STAGE_EGG immediately after kf_creature_
 * screen_init(), via kf_pet_session_state_mutable_for_test() -- a fresh
 * kf_pet_session_init() always starts as an egg (kf/pet.h's kf_pet_init()),
 * and the egg gate (kf_creature_screen.cpp) freezes the creature in place
 * rather than wandering it. Left at EGG, every one of the 300 frames below
 * would measure a stationary bobbing egg instead of the wandering creature
 * this check's dirty-rect/byte budget is actually about -- duplicating
 * run_creature_screen_egg_check() and leaving the genuinely-moving case
 * with no guard of its own, which is exactly what happened once the egg
 * gate landed and this check's own fixture silently stopped noticing. Same
 * fix, same reasoning, as run_creature_screen_death_check()'s own comment
 * on why IT forces a non-egg stage before its first frame too. */
static int run_creature_screen_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-" + std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init mounts the default "
                                      "pack (no creature art in it yet)");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Not an egg -- see this function's own header comment on why. Set
     * before the first frame runs, so every one of the 300 frames below
     * measures the ordinary wander, not the egg's frozen-in-place bob. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->stage = KF_PET_STAGE_CHILD;

    /* The background colour, sampled from the framebuffer itself rather
     * than duplicating kf_creature_screen.cpp's private kBackground
     * constant here (it lives in that file's anonymous namespace, not
     * exposed via kf_creature_screen.h): kf_creature_screen_init() calls
     * kf_creature_screen_enter() before it returns (not as its LAST step --
     * a ready flag is set after it -- but before any frame ever runs
     * either way), which -- after the Important-1 fix -- paints the WHOLE
     * 240x320 panel this colour before the creature's first frame ever
     * runs. Any pixel is a faithful sample at this exact point; (0,0) is as
     * good as any. */
    const kf_color background_color = kf_fb_pixels()[0];

    size_t worst_rects = 0;
    size_t worst_bytes = 0;
    /* Whether any frame's dirty rectangle(s) actually contained a pixel
     * that differs from the background colour -- proof the drawing path
     * (placeholder rect today, real sprite art once the pack carries any)
     * genuinely painted something, not just proof that SOME rectangle got
     * marked dirty. d.count > 0 alone does not tell them apart:
     * kf_fill_rect(g_previous, kBackground) in kf_creature_screen_frame()
     * runs unconditionally every frame and always dirties a rectangle by
     * itself, even with the entire draw call removed -- verified by
     * deleting that block and confirming a rect-count-only version of this
     * check kept passing. Checking the pixels themselves closes that gap:
     * an erase-only frame leaves every dirtied pixel equal to
     * background_color, so this stays false and the check below catches
     * it. */
    bool creature_pixel_ever_drawn = false;
    for (int i = 0; i < 300; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > worst_rects) {
            worst_rects = static_cast<size_t>(d.count);
        }
        const size_t bytes = kf_fb_dirty_bytes();
        if (bytes > worst_bytes) {
            worst_bytes = bytes;
        }

        if (!creature_pixel_ever_drawn) {
            const kf_color *px = kf_fb_pixels();
            for (int r = 0; r < d.count && !creature_pixel_ever_drawn; ++r) {
                const kf_rect rect = d.rects[r];
                for (int16_t y = rect.y0;
                     y < rect.y1 && !creature_pixel_ever_drawn; ++y) {
                    const kf_color *row =
                        px + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
                    for (int16_t x = rect.x0; x < rect.x1; ++x) {
                        if (row[x] != background_color) {
                            creature_pixel_ever_drawn = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    check(creature_pixel_ever_drawn,
          "no dirtied pixel ever differed from the background colour across "
          "300 frames -- drawing nothing (the vacuous case A3's placeholder "
          "rule exists to prevent) would also pass the rect/byte budget "
          "below, and would also pass a rect-COUNT-only version of this "
          "check, since the erase alone always dirties a rectangle; only "
          "sampling actual pixel content catches the drawing path being "
          "removed");

    /* The creature's own erase-then-draw (kf_fill_rect(g_previous, ...)
     * then kf_blit_frame()/kf_blit_frame_mirrored(), Task 6 of the
     * animated-indexed-sprites plan) measures as ONE dirty rectangle in
     * practice, not two: kSpeedPxPerSec (hakoniwaos/src/creature.cpp) is
     * slow enough that a single frame's movement is always far smaller
     * than the 48x48 sprite itself, so the erase rect (g_previous) and the
     * draw rect (now) always overlap or touch and get unioned by
     * kf_fb_mark_dirty() (kf/framebuffer.h's own comment) -- confirmed by
     * the "1 rects" this check actually logs below. The bound stays <=2u
     * anyway, not <=1u: nothing about kf_fill_rect()/kf_blit_frame()'s
     * contract GUARANTEES that merge, only this screen's current, slow
     * wander speed
     * does, so a rect of real headroom stays in the budget rather than a
     * check that would fail the moment the creature moves faster than one
     * sprite width per frame. (This check used to measure 2 rects, not 1
     * -- a redundant kf_fill_rect(kMessBand, kBackground) on the first
     * frame after every screen entry, over a band that was already
     * background, added a second, disjoint rectangle that had nothing to
     * do with the creature at all; see kf_creature_screen.cpp's mess-
     * drawing comment for the fix.)
     *
     * The byte budget below is the same rect's area, not "erase-plus-mess-
     * band" any more either: three 48x48 sprite areas' worth of bytes is
     * generous headroom over the ~48x48 (occasionally a pixel or two
     * larger on each axis, when a frame's step keeps the erase/draw union
     * just bigger than one bare sprite) area that rect actually covers. */
    check(worst_rects <= 2u, "used more than 2 dirty rects in a frame");
    check(worst_bytes <= 48u * 48u * 2u * 3u,
          "dirtied more bytes in a frame than three 48x48 sprites' worth "
          "-- too much for the erase-then-draw budget");

    KF_LOGI(TAG, "creature-screen: worst frame %zu rects, %zu bytes",
            worst_rects, worst_bytes);

    /* Task 5: mess must be STATIC. Eight poops redrawn as eight independent
     * rectangles every frame, plus the creature's own (up to two), would
     * blow past KF_MAX_DIRTY_RECTS and collapse the framebuffer to one
     * screen-sized box every single frame -- see kf_creature_screen.cpp's
     * mess-drawing comment for the exact budget arithmetic. So: force a
     * full house of mess onto the pet via the debug-only mutable accessor
     * (no care action or debug lever reaches poop_count directly), let one
     * frame draw it, then run 100 more frames where NOTHING about the mess
     * changes and confirm none of them cost more than the creature's own
     * per-frame budget (kept at the same "2" bound A1 measured above --
     * loose, but it is exactly the guard that catches mess being redrawn
     * every frame, which is the whole point of this check). */
    /* Counts pixels anywhere in the framebuffer that are not the background
     * colour -- used below to prove mess actually got PAINTED, not just
     * that dirty rectangles stayed within budget. Guards against the same
     * vacuous-pass trap creature_pixel_ever_drawn (above) closes for the
     * creature itself: a screen that never reads poop_count at all would
     * pass a rect-count-only version of the check below just as easily as
     * one that draws mess correctly, since "nothing new" costs zero rects
     * too. */
    auto count_non_background = [&]() -> size_t {
        size_t count = 0;
        const kf_color *px = kf_fb_pixels();
        const size_t total =
            static_cast<size_t>(KF_DISPLAY_WIDTH) * KF_DISPLAY_HEIGHT;
        for (size_t i = 0; i < total; ++i) {
            if (px[i] != background_color) { ++count; }
        }
        return count;
    };
    const size_t non_background_before_mess = count_non_background();

    /* `pet` already points at the live state -- forced off EGG above. */
    pet->poop_count = KF_PET_MAX_POOPS;
    kf_creature_screen_frame(33u); /* the frame that draws them */

    /* Eight poops add real area to the panel beyond whatever the creature
     * (or its placeholder) already accounts for -- comfortably more than
     * this margin even if every poop is small, and far more than the ~1px
     * of drift a single 33ms frame's worth of creature movement alone
     * could plausibly account for (kSpeedPxPerSec = 18, per Controller
     * amendment A1). */
    check(count_non_background() >= non_background_before_mess + 400u,
          "setting poop_count to KF_PET_MAX_POOPS did not visibly add mess "
          "to the framebuffer -- the mess-drawing path is missing or is "
          "not reading pet->poop_count");

    size_t steady_worst = 0;
    for (int i = 0; i < 100; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > steady_worst) {
            steady_worst = static_cast<size_t>(d.count);
        }
    }
    check(steady_worst <= 2u,
          "more than 2 rects with 8 poops standing still -- they are being "
          "redrawn every frame instead of staying static");
    KF_LOGI(TAG, "creature-screen: steady-state worst frame %zu rects with "
                 "a full house of mess",
            steady_worst);

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Task 6: the creature screen's own care-button input. Before this, the
 * only way to move pet->care_actions_taken was the Lua binding
 * (kf_lua_port.cpp) -- the LVGL Home that used to hold Feed/Play
 * (kf_pet_screen.cpp) has been unreachable from a running build since Task
 * 4 (kf_screen_nav.cpp). This proves the five care buttons
 * (kf_creature_screen.cpp's handle_care_buttons()) each reach their own
 * kf_pet_session_* wrapper, that per-action variation counters cycle
 * 0 -> 1 -> 2 -> 0 independently of one another (not one shared counter --
 * see that file's own comment on why a shared counter would be wrong), and
 * that a button-triggered action starts the same reaction-hold countdown
 * Task 4 already wired up for Lua-triggered ones.
 *
 * Drives input via kf_creature_screen_debug_press() rather than a real
 * kf_app_buttons_pressed() read -- the same reason run_screen_nav_check()
 * uses kf_screen_nav_debug_advance()/_home() instead of driving
 * headless_input.cpp's kf_headless_script(): that script is one fixed,
 * frame-indexed sequence shared by every check that goes through
 * kf_app_frame() (headless_determinism among them), and adding a care-
 * button window to it would change what those already-locked golden
 * checksums see. This also means kf_app_init() is never called here, so --
 * same as run_screen_nav_check() -- kf_rng is never reseeded from the
 * entropy HAL and base_trait comes out deterministic from the fixed
 * default seed kf_host_entropy_pin() set in main(). */
static int run_creature_screen_input_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-input-" +
         std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init");

    kf_pet_session_init();
    kf_creature_screen_init();

    const kf_pet_state *pet = kf_pet_session_state();
    uint32_t care_actions_before = pet->care_actions_taken;

    /* Presses one button, checks the action landed (care_actions_taken
     * incremented, last_care_action is the right one) and that the
     * reaction recorded matches kf_pet_reaction_to() for the variation
     * this press SHOULD have used -- proof the variation counter, not just
     * the button-to-action mapping, is doing the right thing. Reads
     * pet->base_trait fresh each call rather than hardcoding an expected
     * value: base_trait is whatever kf_pet_init() rolled for this run's
     * pinned seed, and this check should keep passing however that roll
     * comes out. */
    auto press_and_check = [&](uint32_t button, kf_pet_care_action action,
                                uint8_t expect_variation,
                                const std::string &label) {
        kf_creature_screen_debug_press(button);
        check(pet->care_actions_taken == care_actions_before + 1u,
              (label + ": care_actions_taken did not increment by one")
                  .c_str());
        care_actions_before = pet->care_actions_taken;
        check(pet->last_care_action == static_cast<uint8_t>(action),
              (label + ": last_care_action does not match the button "
                       "pressed")
                  .c_str());
        const uint8_t expect_reaction =
            kf_pet_reaction_to(pet->base_trait, action, expect_variation);
        check(pet->last_reaction == expect_reaction,
              (label + ": last_reaction does not match the variation this "
                       "press should have used -- the per-action variation "
                       "counter is not cycling correctly")
                  .c_str());
    };

    /* Feed three times: variation 0, 1, 2. */
    press_and_check(KF_BTN_A, KF_PET_CARE_FEED, 0u, "feed press 1 (variation 0)");
    press_and_check(KF_BTN_A, KF_PET_CARE_FEED, 1u, "feed press 2 (variation 1)");

    /* Play once, in between two more Feed presses: if the two actions
     * shared one counter instead of each owning its own, this Play press
     * would land on variation 2 (Feed's next value) instead of Play's own
     * fresh 0 -- this is what actually distinguishes "per-action counters"
     * from "one shared counter" rather than merely exercising both. */
    press_and_check(KF_BTN_UP, KF_PET_CARE_PLAY, 0u,
                     "play press (independent variation counter)");

    /* Finish Feed's cycle: variation 2, then wrap back to 0. */
    press_and_check(KF_BTN_A, KF_PET_CARE_FEED, 2u, "feed press 3 (variation 2)");
    press_and_check(KF_BTN_A, KF_PET_CARE_FEED, 0u,
                     "feed press 4 (wraps back to variation 0)");

    /* Rest and Bath, each starting fresh at their own variation 0. */
    press_and_check(KF_BTN_DOWN, KF_PET_CARE_REST, 0u, "rest press (variation 0)");
    press_and_check(KF_BTN_LEFT, KF_PET_CARE_BATH, 0u, "bath press (variation 0)");

    /* A button-triggered action reaches the reaction display exactly the
     * way a Lua-triggered one already did (Task 4): kf_creature_screen_
     * frame() notices pet->care_actions_taken changed since it last looked
     * and starts a 1200ms reaction hold. The bath press just above already
     * moved care_actions_taken, so one dt_ms == 0 frame call (no wander
     * movement, so nothing else about the screen changes) is enough to
     * prove it. */
    kf_creature_screen_frame(0u);
    check(kf_creature_screen_debug_reaction_hold_ms() == 1200u,
          "a button-triggered care action starts the reaction hold, same "
          "as a Lua-triggered one");

    /* Flush (KF_BTN_RIGHT) is the odd one out: no variation, and -- per
     * kf_pet_flush()'s own contract (hakoniwaos/src/pet.cpp) -- it leaves
     * last_reaction/last_care_action exactly as the bath press above left
     * them, since a chore is not a reaction-worthy care action. Set
     * poop_count directly (kf_pet_session_state_mutable_for_test(), the
     * same lever run_pet_mess_check()/creature_screen_check() use to drive
     * the mess-drawing budget without playing out real neglect) so this
     * check can prove flush did its actual job, not just that
     * care_actions_taken moved. */
    kf_pet_session_state_mutable_for_test()->poop_count = 3u;
    const uint8_t last_care_action_before_flush = pet->last_care_action;
    const uint8_t last_reaction_before_flush = pet->last_reaction;
    kf_creature_screen_debug_press(KF_BTN_RIGHT);
    check(pet->care_actions_taken == care_actions_before + 1u,
          "flush press (care_actions_taken incremented)");
    care_actions_before = pet->care_actions_taken;
    check(pet->poop_count == 0u, "flush press actually cleared the mess");
    check(pet->last_care_action == last_care_action_before_flush &&
              pet->last_reaction == last_reaction_before_flush,
          "flush has no variation and no reaction of its own -- "
          "last_care_action/last_reaction stay exactly what the last real "
          "care action (bath) left them, per kf_pet_flush()'s own "
          "contract");

    /* MENU and B stay kf_screen_nav.cpp's alone -- pressing them (and
     * nothing else) here must not move care_actions_taken at all. */
    kf_creature_screen_debug_press(KF_BTN_MENU | KF_BTN_B);
    check(pet->care_actions_taken == care_actions_before,
          "MENU/B are not care buttons and must not trigger a care action");

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Closes the coverage gap resolve_sprite() (kf_creature_screen.cpp) used to
 * record in its own comment: every check above (including run_creature_
 * screen_check() just above this one) mounts the checked-in DEFAULT asset
 * pack, which has no creature art at all, so every kf_assets_get() call
 * that function makes returns nullptr there and only the placeholder-
 * colour fallback ever runs -- kf_blit_frame(), kf_blit_frame_mirrored(),
 * and the "_w_"-not-found -> mirrored "_e_" fallback are never exercised
 * through the real drawing path by anything else in this file.
 *
 * This check points the runtime pack override (kf_host_assets_set_pack_
 * path(), host_assets.h) at examples/creature_demo/assets.kfpack instead --
 * the real (if placeholder-tier) egg art the art-naming task generated,
 * egg_idle_{s,e,n} and no "_w_" -- restoring the override to the
 * default before returning so nothing later in this process inherits it.
 * Deliberately does NOT touch KF_ASSET_PACK or examples/hello_sprite/
 * assets.kfpack: headless_determinism, headless_fullscreen and asset_
 * pipeline_check checksum rendered output (or the pack file itself)
 * against that untouched default and would fail legitimately if either
 * moved -- see simulator/CMakeLists.txt's KF_CREATURE_DEMO_PACK_PATH
 * comment for the compile-time path this reads.
 *
 * A fresh pet is stage EGG (kf_pet_init(), kf/pet.h) by default, which the
 * egg's single-state, three-direction design (kf_creature_sprite_name()
 * collapsing every pose to "egg_idle_<dir>") makes the simplest fixture
 * here -- no need to advance the pet through any stage first, and
 * poop_count starts at 0, so the mess-drawing path never adds anything
 * else to the panel to confuse the pixel sampling below.
 *
 * Drives the creature's facing directly via kf_creature_screen_debug_set_
 * direction() rather than waiting on the wander's own RNG to visit every
 * direction eventually. Every kf_creature_screen_frame() call below passes
 * dt_ms == 0, so kf_creature_update() (hakoniwaos/src/creature.cpp) never
 * recomputes ::dir out from under the forced value (it returns immediately
 * on dt_ms == 0, touching neither position nor facing) -- which also means
 * the creature's on-screen position never changes across this whole check,
 * so kf_creature_screen_debug_bounds() is sampled once and reused: it is
 * the exact rectangle kf_blit_frame()/kf_blit_frame_mirrored() (Task 6 of
 * the animated-indexed-sprites plan; kf_blit()/kf_blit_mirrored() were what
 * drew here before that landed) draw into for every direction below, not a
 * bounding box inferred from pixel content (which would only be as tight as
 * this particular sprite's transparent margins
 * happen to be, and is not something a test should assume is symmetric).
 *
 * dt_ms == 0 also means the animation cursor (kf_creature::anim, kf/
 * creature.h) never advances here either: kf_creature_tick_anim() is a
 * no-op on dt_ms == 0 regardless of which of its two callers (the wander
 * branch, gated off by the same dt_ms == 0 in kf_creature_update(), or the
 * egg branch calling it directly) would otherwise have reached it. Moot for
 * this check anyway -- every sprite examples/creature_demo/assets.kfpack
 * ships is a single frame, so kf_creature_anim_wrap() would clamp the
 * cursor to 0 even if it had moved. */
static int run_creature_screen_sprite_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-sprites-" +
         std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();

    kf_host_assets_set_pack_path(KF_CREATURE_DEMO_PACK_PATH);
    check(kf_assets_init() == KF_OK,
          "kf_assets_init mounts examples/creature_demo/assets.kfpack");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Same reasoning as run_creature_screen_check()'s own background_color
     * sample: kf_creature_screen_init() calls kf_creature_screen_enter(),
     * which paints the whole panel this colour before anything else runs,
     * so any pixel is a faithful sample at this exact point. */
    const kf_color background_color = kf_fb_pixels()[0];

    /* Fixed for the whole check -- see this function's own header comment
     * for why the creature never moves from here. */
    const kf_rect creature_rect = kf_creature_screen_debug_bounds();
    check(!kf_rect_is_empty(creature_rect),
          "kf_creature_screen_debug_bounds() returned an empty rect");

    /* egg_idle_s et al. are drawn via kf_blit_frame()'s colour-key skip
     * (frame 0 -- egg_idle now carries 9 frames, see
     * .superpowers/sdd/first-animations-report.md, but every draw() below
     * passes dt_ms == 0, so g_creature.anim.frame never advances off 0;
     * this is still kf_blit()'s own frame-0 equivalence per kf/blit.h, not
     * a claim that the sprite itself is single-frame), not a flat
     * kf_fill_rect() -- see kf_ingest_sprites.py's alpha-to-colour-key
     * resolution in the art-naming report -- so a real sprite's rect will
     * contain more than one non-background colour. kPlaceholderColor
     * (kf_creature_screen.cpp) is a flat kf_fill_rect() over the whole
     * creature rect instead: every pixel in it is this ONE colour and
     * nothing else. Reproduced here by value, the same "mirror a private
     * constant across a file boundary" pattern headless_main.cpp already
     * uses for kBackground in run_screen_nav_check() above. */
    constexpr kf_color kKnownPlaceholderColor = KF_RGB(255, 0, 128);

    /* True if creature_rect holds at least one pixel that is neither
     * background nor the known placeholder colour -- the "genuinely more
     * than one flat fill" signature real sprite art has and kPlaceholder-
     * Color's single kf_fill_rect() cannot. Must be called before the NEXT
     * draw erases creature_rect's contents. */
    auto has_real_sprite_content = [&]() -> bool {
        const kf_color *px = kf_fb_pixels();
        for (int16_t y = creature_rect.y0; y < creature_rect.y1; ++y) {
            const kf_color *row =
                px + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
            for (int16_t x = creature_rect.x0; x < creature_rect.x1; ++x) {
                if (row[x] != background_color &&
                    row[x] != kKnownPlaceholderColor) {
                    return true;
                }
            }
        }
        return false;
    };

    /* Copies creature_rect's current pixels out before the next draw
     * overwrites them -- row-major, for the mirrored-pixel comparison
     * further down. */
    auto snapshot = [&]() -> std::vector<kf_color> {
        const kf_color *px = kf_fb_pixels();
        const size_t w =
            static_cast<size_t>(creature_rect.x1 - creature_rect.x0);
        const size_t h =
            static_cast<size_t>(creature_rect.y1 - creature_rect.y0);
        std::vector<kf_color> out(w * h);
        for (size_t row = 0; row < h; ++row) {
            const kf_color *src =
                px + static_cast<size_t>(
                         creature_rect.y0 + static_cast<int16_t>(row)) *
                         KF_DISPLAY_WIDTH +
                static_cast<size_t>(creature_rect.x0);
            std::copy(src, src + w, out.begin() + static_cast<long>(row * w));
        }
        return out;
    };

    /* Forces `want_dir` and draws one frame -- dt_ms == 0 (see this
     * function's own header comment for why). */
    auto draw = [&](kf_creature_direction want_dir) {
        kf_creature_screen_debug_set_direction(want_dir);
        kf_creature_screen_frame(0u);
    };

    /* S, E and N: a known sprite name (egg_idle_<dir>) resolves and
     * kf_blit_frame()'s real pixels, not the placeholder colour -- three of
     * resolve_sprite()'s non-null branches (S and N repeat the "found on
     * first try" branch E also takes; the point is proving the direction
     * actually reaches the pack, not enumerating branches redundantly). */
    draw(KF_CREATURE_DIR_S);
    check(has_real_sprite_content(),
          "facing S drew only background and/or the placeholder colour -- "
          "egg_idle_s should have resolved from examples/creature_demo/"
          "assets.kfpack and kf_blit_frame()'d real sprite pixels");

    /* Same direction again, immediately: exercises resolve_sprite()'s
     * cache-hit branch (SpriteCache::valid, matching requested_name) --
     * every OTHER check in this file only ever hits that branch with a
     * cached nullptr, since their pack has no creature art to find. */
    draw(KF_CREATURE_DIR_S);
    check(has_real_sprite_content(),
          "facing S a second time in a row (the cache-hit path) drew only "
          "background and/or the placeholder colour");

    draw(KF_CREATURE_DIR_N);
    check(has_real_sprite_content(),
          "facing N drew only background and/or the placeholder colour -- "
          "egg_idle_n should have resolved and drawn real sprite "
          "pixels");

    draw(KF_CREATURE_DIR_E);
    check(has_real_sprite_content(),
          "facing E drew only background and/or the placeholder colour -- "
          "egg_idle_e should have resolved and drawn real sprite "
          "pixels");
    const std::vector<kf_color> east_pixels = snapshot();

    /* W: the demo pack ships no egg_idle_w, so this is resolve_sprite()'s
     * west-first-fallback branch -- kf_assets_get("egg_idle_w") misses,
     * then kf_assets_get("egg_idle_e") hits and `mirrored` is set,
     * exercising kf_blit_frame_mirrored() for the first time anywhere in
     * this file's whole test suite. */
    draw(KF_CREATURE_DIR_W);
    check(has_real_sprite_content(),
          "facing W drew only background and/or the placeholder colour -- "
          "with no egg_idle_w in the pack this should have fallen back "
          "to egg_idle_e drawn mirrored");
    const std::vector<kf_color> west_pixels = snapshot();

    /* Both snapshots cover the exact same fixed creature_rect (the
     * creature never moved -- see this function's own header comment), so
     * kf_blit_frame_mirrored()'s W-fallback output must be E's own pixels
     * reversed column-for-column across that whole rect, including
     * whatever colour-keyed (transparent) columns fall inside it: kf/
     * blit.h's own comment says the bounding box is identical to kf_blit()'s
     * and only the columns are read back-to-front, and a colour-keyed
     * source column leaves the framebuffer's background pixel underneath
     * untouched at both its normal and its mirrored screen position, so
     * this equality has to hold there too, not only across the visible
     * silhouette. */
    const size_t w = static_cast<size_t>(creature_rect.x1 - creature_rect.x0);
    const size_t h = static_cast<size_t>(creature_rect.y1 - creature_rect.y0);
    bool mirrored_ok = east_pixels.size() == west_pixels.size();
    for (size_t y = 0; mirrored_ok && y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            if (east_pixels[y * w + x] != west_pixels[y * w + (w - 1u - x)]) {
                mirrored_ok = false;
                break;
            }
        }
    }
    check(mirrored_ok,
          "facing W's pixels are not facing E's pixels reversed "
          "column-for-column across the creature's rect -- the west "
          "fallback should draw egg_idle_e via kf_blit_frame_mirrored(), "
          "not kf_blit_frame() or any other transform");

    KF_LOGI(TAG,
            "creature-screen-sprites: S/E/N resolved real sprite pixels, W "
            "fell back to E mirrored");

    /* Restore the default before shutdown -- see this function's own
     * header comment for why, even though nothing else runs in this
     * process after this check returns today. */
    kf_host_assets_set_pack_path(nullptr);

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves three of this task's four enumerated testable claims at once,
 * because they all fall out of the same fresh-pet-is-an-egg fixture and
 * the same per-frame loop:
 *
 *   1. the egg does not wander -- kf_creature_screen_debug_bounds() must
 *      report the exact same rect on the very last frame as on the very
 *      first, across many frames of real elapsed time, for a stage that
 *      used to wander like every other one (the bug this task fixes);
 *   2. the bob stays inside the dirty-rect budget -- reusing the same
 *      worst_rects measurement run_creature_screen_check() already applies
 *      to a wandering creature, applied here to a bobbing one instead,
 *      still <=2u;
 *   3. the on-screen button guide is never redrawn per frame -- across the
 *      same loop, no frame's dirty rectangles may touch the reserved
 *      band, y=[260,320), at all.
 *
 * The fourth claim, the death scene centring, has its own fixture below
 * (run_creature_screen_death_check()) -- a dead pet is a different pet
 * than a fresh egg, and the two do not share a meaningful setup.
 *
 * Also checks the bob is not a no-op: kf_creature_screen_debug_egg_bob_
 * offset_y() must take at least one nonzero value across the run, or a
 * broken/always-zero wave would pass claim 2 above completely vacuously
 * (zero movement trivially never blows a rect budget) the same way
 * creature_pixel_ever_drawn guards run_creature_screen_check() above
 * against a vacuous "drew nothing" pass. And -- what kf_creature_screen.h's
 * own comment on kf_creature_screen_debug_egg_bob_offset_y() promises this
 * accessor is FOR, not just that it moves -- that every value it ever
 * returns across the run stays within its intended amplitude
 * (kKnownEggBobAmplitudePx below, kf_creature_screen.cpp's private
 * kEggBobAmplitudePx reproduced by value, the same cross-file-constant
 * pattern this file already uses for kKnownPlaceholderColor/kShrine
 * PlaceholderSize elsewhere): a wave that overshot -- a bad multiplier, an
 * off-by-one in egg_bob_offset_y()'s quarter-period arithmetic -- would
 * still pass "nonzero at least once" and could still pass the rect budget
 * (the budget only cares that consecutive frames don't jump far, not that
 * the whole wave stays small), so neither of those alone would have caught
 * an amplitude regression. */
static int run_creature_screen_egg_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-egg-" +
         std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init");

    kf_pet_session_init();
    kf_creature_screen_init();

    const kf_pet_state *pet = kf_pet_session_state();
    check(pet->stage == KF_PET_STAGE_EGG,
          "a fresh pet is an egg -- this check's whole fixture depends on "
          "it (kf_pet_init(), kf/pet.h)");

    const kf_rect initial_bounds = kf_creature_screen_debug_bounds();
    check(!kf_rect_is_empty(initial_bounds),
          "kf_creature_screen_debug_bounds() returned an empty rect for a "
          "freshly-initialised egg");

    /* Same reasoning as draw_care_guide()'s own comment: the guide is
     * painted once, by kf_creature_screen_init()'s call into kf_creature_
     * screen_enter(), before this loop ever runs -- so it must already be
     * visible on screen right now, and this is the one point in the check
     * where sampling for it proves anything (every dirty-rect assertion
     * below is about frames AFTER this point never touching it again, not
     * about whether it was ever drawn in the first place). Scans slot 0's
     * whole cell -- x=[0,48) (kGuideSlotWidth, kf_creature_screen.cpp-
     * private, reproduced here by value like kBackground/
     * kKnownPlaceholderColor elsewhere in this file), y=[260,268)
     * (kGuideTextY=286 down through one KF_FONT_CELL_H=8 row -- actually
     * the whole reserved band down to 320 is fair game, but one glyph row
     * is enough) -- for ANY pixel that is fg=KF_BLACK, rather than
     * assuming a specific pixel inside "1:FEED"'s glyph shapes happens to
     * be foreground; kf_text_draw()'s bg fill (kBackground) covers the
     * whole cell margin too, so most of this rectangle is background even
     * where a real label was drawn, and only the actual glyph strokes are
     * KF_BLACK. */
    bool guide_pixel_found = false;
    {
        const kf_color *px = kf_fb_pixels();
        for (int16_t y = 260; y < 320 && !guide_pixel_found; ++y) {
            const kf_color *row = px + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
            for (int16_t x = 0; x < 48; ++x) {
                if (row[x] == KF_BLACK) {
                    guide_pixel_found = true;
                    break;
                }
            }
        }
    }
    check(guide_pixel_found,
          "the care-button guide's first label was not drawn into the "
          "reserved band on screen entry -- see draw_care_guide() "
          "(kf_creature_screen.cpp)");

    /* kf_creature_screen.cpp's private kEggBobAmplitudePx, reproduced by
     * value -- see this function's own header comment above for why (same
     * pattern as kKnownPlaceholderColor/kShrinePlaceholderSize elsewhere in
     * this file). The wave (egg_bob_offset_y()) is documented never to
     * exceed this in magnitude; amplitude_ever_exceeded below is what
     * actually holds it to that promise instead of just trusting it. */
    constexpr int16_t kKnownEggBobAmplitudePx = 2;

    size_t worst_rects = 0;
    bool bob_ever_nonzero = false;
    bool amplitude_ever_exceeded = false;
    int16_t last_offset = kf_creature_screen_debug_egg_bob_offset_y();
    bool guide_band_ever_touched = false;
    for (int i = 0; i < 600; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > worst_rects) {
            worst_rects = static_cast<size_t>(d.count);
        }
        for (int r = 0; r < d.count; ++r) {
            if (d.rects[r].y1 > 260) {
                guide_band_ever_touched = true;
            }
        }
        const int16_t offset = kf_creature_screen_debug_egg_bob_offset_y();
        if (offset != 0) {
            bob_ever_nonzero = true;
        }
        if (offset > kKnownEggBobAmplitudePx ||
            offset < -kKnownEggBobAmplitudePx) {
            amplitude_ever_exceeded = true;
        }
        last_offset = offset;
    }
    (void)last_offset;

    check(worst_rects <= 2u,
          "an egg bobbing in place used more than 2 dirty rects in a "
          "frame -- the same budget a wandering creature already holds "
          "to");
    KF_LOGI(TAG, "creature-screen-egg: worst frame %zu rects", worst_rects);

    check(!amplitude_ever_exceeded,
          "the egg bob offset exceeded its intended amplitude at least "
          "once across 600 frames -- kf_creature_screen_debug_egg_bob_"
          "offset_y()'s own header comment (kf_creature_screen.h) promises "
          "a check can rely on this staying bounded");

    check(bob_ever_nonzero,
          "the egg bob offset was 0 for all 600 frames -- either the wave "
          "is broken or dt_ms is not reaching egg_bob_offset_y() at all, "
          "which would make the dirty-rect check above pass vacuously "
          "(zero movement never blows a rect budget)");

    check(!guide_band_ever_touched,
          "a frame's dirty rectangles touched the reserved guide band "
          "(y>=260) after screen entry -- the guide must be static, drawn "
          "once by kf_creature_screen_enter(), never redrawn from kf_"
          "creature_screen_frame()");

    const kf_rect final_bounds = kf_creature_screen_debug_bounds();
    check(final_bounds.x0 == initial_bounds.x0 &&
              final_bounds.y0 == initial_bounds.y0 &&
              final_bounds.x1 == initial_bounds.x1 &&
              final_bounds.y1 == initial_bounds.y1,
          "the egg's own position (kf_creature_bounds(), NOT the bobbed "
          "draw position -- see kf_creature_screen_debug_bounds()'s own "
          "comment) moved across 600 frames of a stage that must not "
          "wander at all");

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the fourth of this task's enumerated testable claims: the death
 * scene centres a single shrine in the field once pet->dead is true, and
 * nothing about it wanders or redraws once painted.
 *
 * Mounts the checked-in DEFAULT asset pack (no shrine_idle_s in it, the
 * same "art may not exist yet" situation every other check in this file
 * that uses the default pack already lives with -- see kPlaceholderColor's
 * own comment), so this exercises the placeholder-rectangle fallback
 * path, not real shrine art. That is deliberate, not a gap: the concurrent
 * art-generation task may not have shipped shrine_idle_s into the
 * default pack by the time this runs, and this check has to hold either
 * way -- exactly the same reasoning run_creature_screen_check() already
 * applies to the creature's own placeholder fallback. */
static int run_creature_screen_death_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-death-" +
         std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init mounts the default "
                                      "pack (no shrine art in it yet)");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Same sampling trick run_creature_screen_check() uses for its own
     * background_color: any pixel is a faithful sample of the background
     * right after kf_creature_screen_init(), which paints the whole panel
     * before anything else runs. */
    const kf_color background_color = kf_fb_pixels()[0];

    /* Walk the creature a little first, as a LIVING pet (not an egg, so
     * this exercises the ordinary wander, not the egg gate), before it
     * dies -- proving the death branch actually stops something that was
     * genuinely moving, not a creature that never left its start rect in
     * the first place. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->stage = KF_PET_STAGE_CHILD;
    for (int i = 0; i < 30; ++i) {
        kf_creature_screen_frame(33u);
    }

    pet->dead = true;
    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u); /* the one frame that paints the shrine */

    constexpr kf_color kKnownPlaceholderColor = KF_RGB(255, 0, 128);

    /* Centred: kShrinePlaceholderSize is 48 (kf_creature_screen.cpp,
     * private -- reproduced here by value, the same cross-file-constant
     * pattern used throughout this file), kField is 240 wide, so the
     * placeholder rect spans x=[96,144). The exact centre pixel (120, *)
     * must be the placeholder colour; well outside that span (e.g. x=10,
     * a corner the creature was free to wander through moments ago, and
     * where the field's OWN background would show if nothing were
     * centred, or a stray creature sprite would show if the wander had
     * not actually stopped) must still be plain background. This proves
     * centring directionally without duplicating centered_in_field()'s
     * exact arithmetic in the test. */
    const kf_color *px = kf_fb_pixels();
    check(px[150 * KF_DISPLAY_WIDTH + 120] == kKnownPlaceholderColor,
          "the shrine placeholder was not drawn centred in the field -- "
          "expected the placeholder colour at the field's centre pixel");
    check(px[150 * KF_DISPLAY_WIDTH + 10] == background_color,
          "something was drawn far from the field's centre when the pet "
          "died -- the shrine (or its placeholder) must be centred, not "
          "spread across the field, and nothing else should be wandering "
          "any more");

    /* Static once painted: further frames must draw nothing further at
     * all -- zero dirty rects, not just a small number, since there is
     * nothing left for a dead pet's scene to update frame to frame. */
    size_t steady_worst = 0;
    for (int i = 0; i < 100; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > steady_worst) {
            steady_worst = static_cast<size_t>(d.count);
        }
    }
    check(steady_worst == 0u,
          "a frame after the shrine was already painted still dirtied a "
          "rectangle -- the death scene must be a one-time static draw, "
          "exactly like the mess and the button guide");

    KF_LOGI(TAG, "creature-screen-death: shrine centred and static");

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Closes the testing gap a review of the stage-jump debug lever
 * (kf_pet_session_debug_jump_to_stage(), added for sdl_debug_window.cpp's
 * stage-jump buttons) found: every check above either jumps stage without
 * ever rendering a frame afterward (run_pet_debug_jump_check(), which
 * never touches kf_creature_screen_frame() at all), or renders frames
 * without ever jumping (every creature_screen_* check above, each built
 * from either a bare fresh pet or one hand-set field at a time). Neither
 * shape can catch a bug that only exists at the SEAM between the two --
 * and two of this task's three Important defects lived exactly there:
 * kf_creature_screen.cpp's per-frame code assumed a dead pet stays dead,
 * and assumed an egg has always been an egg, both of which the jump lever
 * makes routine. This check drives both seams the same fixture, back to
 * back, on the same live pet:
 *
 *   A. Jump to a stage AFTER a death, then render. Before the fix, the
 *      shrine's own g_drawn_dead flag never got told the revive happened,
 *      so the shrine stayed painted at the field's centre on top of the
 *      now-living creature until the next screen re-entry. Proven here by
 *      confirming the shrine really was painted before the jump (the same
 *      pixel check run_creature_screen_death_check() already uses), then
 *      confirming the very next rendered frame's dirty rectangles fully
 *      cover the shrine's own rect -- proof a repaint actually reached
 *      that exact region, which is what the fix does (kf_creature_
 *      screen.cpp's revive check, just below its death branch). Checking
 *      rect COVERAGE rather than final pixel content is deliberate: the
 *      shrine and the ordinary placeholder creature share the exact same
 *      fallback colour (kPlaceholderColor), so a pixel-content check at
 *      the shrine's old position could pass "by accident" if the revived
 *      creature happened to be drawn there too, without ever proving a
 *      repaint happened at all.
 *
 *   B. Jump BACK to Egg from a creature that has been wandering (not from
 *      a fresh boot, where the egg's frozen position and the field's
 *      centre are the same thing by construction and this bug cannot
 *      show up). Before the fix, the egg gate froze g_creature.x/y
 *      wherever the wander had last left them -- anywhere in the field,
 *      not the centre -- producing an off-centre bobbing egg and, at the
 *      field's bottom edge, a drawn rect breaking this file's own
 *      y=[0,260) invariant. Proven here two ways: kf_creature_screen_
 *      debug_bounds() must read back to EXACTLY the same rect a fresh
 *      egg starts at (captured once, at the very top of this check,
 *      before anything has had a chance to move), and no dirty rectangle
 *      across a further run of bobbing frames may ever cross y=260.
 *
 * Reuses ONE fixture for both, in order, rather than two separate ones:
 * part A's jump target (CHILD, alive) is exactly the wandering, non-egg
 * state part B's own "from a wandered creature" setup needs anyway, so
 * running them back to back means part B does not need to re-establish
 * it from scratch. */
static int run_creature_screen_debug_jump_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-debug-jump-" +
         std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init mounts the default "
                                      "pack (no creature/shrine art in it "
                                      "yet)");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Captured once, before anything below ever moves the creature or
     * changes the pet's stage -- this IS "exactly where a fresh egg
     * starts", the value part B's jump-back-to-egg re-centre must
     * reproduce. Same technique run_creature_screen_egg_check() already
     * uses for its own initial_bounds/final_bounds comparison. */
    const kf_rect fresh_egg_bounds = kf_creature_screen_debug_bounds();
    check(!kf_rect_is_empty(fresh_egg_bounds),
          "kf_creature_screen_debug_bounds() returned an empty rect for a "
          "freshly-initialised egg");

    /* ---------------------------------------------------------------
     * Part A: jump to a stage after a death, then render.
     * --------------------------------------------------------------- */

    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    /* Not an egg: the egg gate means an egg never wanders in the first
     * place, which would make "the creature was somewhere other than the
     * shrine's own rect when it died" true only by luck rather than by
     * construction. CHILD wanders for real. */
    pet->stage = KF_PET_STAGE_CHILD;
    for (int i = 0; i < 30; ++i) {
        kf_creature_screen_frame(33u);
    }

    pet->dead = true;
    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u); /* the one frame that paints the shrine */

    /* Same known-by-construction placeholder colour and centred rect
     * run_creature_screen_death_check() already establishes and checks --
     * see kShrinePlaceholderSize/centered_in_field() in kf_creature_
     * screen.cpp for the arithmetic this reproduces by value. Confirms
     * this check's OWN fixture actually reached the state the rest of
     * part A assumes, before relying on it. */
    constexpr kf_color kKnownPlaceholderColor = KF_RGB(255, 0, 128);
    constexpr kf_rect kKnownShrineRect = {96, 106, 144, 154};
    const kf_color *px = kf_fb_pixels();
    check(px[130 * KF_DISPLAY_WIDTH + 120] == kKnownPlaceholderColor,
          "the shrine placeholder was not drawn at the field's centre "
          "before the jump -- this check's fixture for part A depends on "
          "it (see run_creature_screen_death_check() for the same "
          "assertion in isolation)");

    /* The jump: the same effect sdl_debug_window.cpp's "Child" stage-jump
     * button has on a pet that is currently dead -- kf_pet_session_debug_
     * jump_to_stage() calls kf_pet_init() internally (kf_pet_session.cpp),
     * which is one of exactly two places in this binary that ever clears
     * pet->dead once set (kf_pet_session_debug_reset() is the other). */
    kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_CHILD, 0u, 0u);
    check(!kf_pet_session_state()->dead,
          "kf_pet_session_debug_jump_to_stage() did not clear pet->dead -- "
          "this check's fixture for part A is broken, not the code under "
          "test");

    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u); /* the revive-render frame */

    /* The fix (kf_creature_screen.cpp's revive check, just below its death
     * branch) repaints the WHOLE field, so this frame's dirty rectangles
     * must fully cover the shrine's own rect regardless of where the
     * revived creature ends up being drawn this same frame -- see this
     * function's own header comment on why rect COVERAGE, not final pixel
     * content, is what actually distinguishes "repainted" from "the
     * creature happened to land here too". */
    const kf_dirty_rects revive_dirty = kf_fb_dirty_rects();
    bool shrine_rect_repainted = false;
    for (int r = 0; r < revive_dirty.count; ++r) {
        const kf_rect &rc = revive_dirty.rects[r];
        if (rc.x0 <= kKnownShrineRect.x0 && rc.y0 <= kKnownShrineRect.y0 &&
            rc.x1 >= kKnownShrineRect.x1 && rc.y1 >= kKnownShrineRect.y1) {
            shrine_rect_repainted = true;
        }
    }
    check(shrine_rect_repainted,
          "reviving after death did not repaint the shrine's own rect "
          "(x=[96,144), y=[106,154)) on the very next rendered frame -- "
          "the shrine would otherwise sit painted over the now-living "
          "creature until the next kf_creature_screen_enter()");

    KF_LOGI(TAG, "creature-screen-debug-jump: part A (revive after death) "
                 "repainted the shrine's rect");

    /* ---------------------------------------------------------------
     * Part B: jump back to Egg from a creature that has wandered.
     * --------------------------------------------------------------- */

    /* Still CHILD and alive from part A's jump -- wander it further so
     * there is no doubt it has genuinely moved away from centre before
     * the jump back to Egg below, the same "prove it actually moved, not
     * vacuously already where the assertion wants it" reasoning run_
     * creature_wander_check() and run_creature_screen_egg_check() both
     * already apply to their own movement claims. */
    const kf_rect before_more_wander = kf_creature_screen_debug_bounds();
    for (int i = 0; i < 200; ++i) {
        kf_creature_screen_frame(33u);
    }
    const kf_rect wandered_bounds = kf_creature_screen_debug_bounds();
    check(wandered_bounds.x0 != before_more_wander.x0 ||
              wandered_bounds.y0 != before_more_wander.y0,
          "the creature never moved across 200 further frames of CHILD "
          "wander -- this check's fixture for part B needs it to have "
          "genuinely wandered for the jump-back-to-egg re-centre to prove "
          "anything");

    kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_EGG, 0u, 0u);
    check(kf_pet_session_state()->stage == KF_PET_STAGE_EGG,
          "kf_pet_session_debug_jump_to_stage(KF_PET_STAGE_EGG, ...) did "
          "not land on KF_PET_STAGE_EGG -- this check's fixture for part B "
          "is broken, not the code under test");

    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u); /* the re-centre-on-transition frame */

    const kf_rect after_jump_to_egg = kf_creature_screen_debug_bounds();
    check(after_jump_to_egg.x0 == fresh_egg_bounds.x0 &&
              after_jump_to_egg.y0 == fresh_egg_bounds.y0 &&
              after_jump_to_egg.x1 == fresh_egg_bounds.x1 &&
              after_jump_to_egg.y1 == fresh_egg_bounds.y1,
          "jumping back to Egg from a wandered creature did not re-centre "
          "it -- kf_creature_screen_debug_bounds() must read back exactly "
          "what a fresh egg starts at (see kf_creature_screen.cpp's "
          "g_was_egg re-centre, just below the egg gate)");

    /* And: across a further run of egg-bobbing frames, no dirty rectangle
     * may ever cross y=260 -- the invariant this file's own header
     * comment states as a hard rule, and the exact one an off-centre egg
     * near the field's bottom edge could break before the re-centre fix.
     * Position no longer matters here (re-centring makes it always the
     * same, regardless of where the wander above happened to leave it),
     * so this holds deterministically rather than depending on having
     * wandered close enough to the edge by chance. */
    bool overflowed_field = false;
    for (int i = 0; i < 200; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        for (int r = 0; r < d.count; ++r) {
            if (d.rects[r].y1 > 260) {
                overflowed_field = true;
            }
        }
    }
    check(!overflowed_field,
          "a dirty rectangle drawn while the pet was an egg reached past "
          "y=260 -- see kf_creature_screen.cpp's own y=[0,260) invariant "
          "and the egg re-centre fix that keeps a jumped-back egg from "
          "breaking it");

    KF_LOGI(TAG, "creature-screen-debug-jump: part B (jump back to egg) "
                 "re-centred and stayed within the field");

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Task 9 of the hardware bring-up plan (docs/superpowers/plans/
 * 2026-08-11-hardware-bringup.md). The owner's own words, after the game ran
 * on real hardware: "I also can't see the pet's stats anywhere now like how
 * hungry/tired etc." The old LVGL pet screen (kf_pet_screen.cpp) had bars for
 * hunger, happiness and energy; the creature screen replaced it and left the
 * reserved band (y=[260,320)) showing only the already-static care-button
 * guide. This proves the stats band this task adds against the three claims
 * that actually matter:
 *
 *   1. all three bars are really painted -- real framebuffer pixels, not
 *      just an internal counter -- the moment the screen is entered;
 *   2. a run of frames where NO need changes costs nothing extra against
 *      the creature's own <=2u dirty-rect budget (run_creature_screen_
 *      check()'s own bound, reused here rather than invented fresh, because
 *      that IS the claim being tested: the band adds zero to it while
 *      steady);
 *   3. a need that changes enough to move its bar's QUANTISED width DOES
 *      cause exactly that bar -- and only that bar -- to redraw on the very
 *      next frame.
 *
 * Claim 2 alone cannot tell "redraws only on change" apart from "never
 * redraws at all" -- a mechanism that draws nothing, ever, would also pass
 * it, which is exactly the vacuous-pass trap this task's own brief names:
 * "a test that only checks 'something was drawn' will pass against code
 * that redraws every frame". Claim 3 is what closes that gap from the other
 * side, and claim 1 is what stops both of them passing against a bar that
 * is never drawn in the first place. */
static int run_creature_screen_stats_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-stats-" +
         std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_arena_init_all();
    kf_fb_init();
    check(kf_assets_init() == KF_OK, "kf_assets_init");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Not an egg -- CHILD keeps this fixture's own creature budget
     * identical to run_creature_screen_check()'s, which is exactly the
     * <=2u bound claim 2 below reuses; nothing about which stage the pet is
     * in matters to whether the STATS band redraws. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->stage = KF_PET_STAGE_CHILD;

    /* Claim 1: all three bars were actually painted on entry. A fresh pet
     * (kf_pet_init(), kf/pet.h) starts every need at KF_PET_MILLIPERCENT_
     * MAX, so every bar's own rect should already read back as fully
     * filled -- compared against the rect's OWN width, not a hardcoded
     * pixel count this file would otherwise have to keep in sync with
     * kf_creature_screen.cpp's private bar width by hand. */
    for (int i = 0; i < 3; ++i) {
        const kf_rect r = kf_creature_screen_debug_stat_bar_bounds(i);
        check(!kf_rect_is_empty(r),
              "kf_creature_screen_debug_stat_bar_bounds() returned an "
              "empty rect for a valid bar index -- the band was never "
              "laid out");
        check(r.y0 >= 260 && r.y1 <= 320,
              "a stat bar's own rect fell outside the reserved band, "
              "y=[260,320)");
        const int16_t full_width = static_cast<int16_t>(r.x1 - r.x0);
        check(kf_creature_screen_debug_stat_bar_filled_px(i) == full_width,
              "a stat bar was not fully filled right after screen entry, "
              "for a fresh pet whose needs all start full");
    }

    /* Same claim, sampled as an actual framebuffer pixel rather than
     * trusting the internal counter above -- the same "content, not just a
     * dirty flag" discipline run_creature_screen_check()'s own creature_
     * pixel_ever_drawn already applies to the creature's sprite. The
     * hunger bar's own left edge must show ITS fill colour, not the plain
     * field background, once it is genuinely full. */
    const kf_rect hunger_rect = kf_creature_screen_debug_stat_bar_bounds(0);
    const kf_color background = kf_fb_pixels()[0];
    const kf_color hunger_sample =
        kf_fb_pixels()[static_cast<size_t>(hunger_rect.y0) *
                            KF_DISPLAY_WIDTH +
                        static_cast<size_t>(hunger_rect.x0)];
    check(hunger_sample != background,
          "the hunger bar's own rect still shows the plain field "
          "background after screen entry -- the bar was never actually "
          "painted onto the framebuffer");

    /* Claim 2: a steady run of frames where no need changes must cost
     * nothing extra against the creature's own dirty-rect budget. Nothing
     * in this loop ever calls kf_pet_advance() or otherwise touches a
     * need, so every one of these 120 frames is "steady" by construction. */
    size_t steady_worst_rects = 0;
    for (int i = 0; i < 120; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > steady_worst_rects) {
            steady_worst_rects = static_cast<size_t>(d.count);
        }
    }
    check(steady_worst_rects <= 2u,
          "a stat bar redrew on a frame where no need actually changed -- "
          "see update_stat_bar()'s own comment (kf_creature_screen.cpp) on "
          "comparing the QUANTISED bar width, not the raw millipercent, "
          "which is exactly what this bound would catch a regression of");
    KF_LOGI(TAG, "creature-screen-stats: steady-state worst frame %zu rects",
            steady_worst_rects);

    /* Claim 3: changing ONE need enough to move its bar's quantised width
     * does cause that bar -- and only that bar -- to redraw on the very
     * next frame. Halving hunger moves it by roughly half the bar's own
     * pixel width, far more than one quantisation step, so this is not
     * relying on happening to land on a boundary by luck. */
    const int16_t happiness_before =
        kf_creature_screen_debug_stat_bar_filled_px(1);
    const int16_t energy_before =
        kf_creature_screen_debug_stat_bar_filled_px(2);
    const int16_t hunger_before =
        kf_creature_screen_debug_stat_bar_filled_px(0);
    pet->hunger_mp = pet->hunger_mp / 2u;

    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u);
    const kf_dirty_rects changed = kf_fb_dirty_rects();

    bool band_touched = false;
    for (int r = 0; r < changed.count; ++r) {
        if (changed.rects[r].y1 > 260) { band_touched = true; }
    }
    check(band_touched,
          "halving pet->hunger_mp did not dirty anything in the reserved "
          "stats band (y>=260) on the very next frame -- the bar never "
          "redrew");

    const int16_t hunger_after =
        kf_creature_screen_debug_stat_bar_filled_px(0);
    check(hunger_after < hunger_before,
          "the hunger bar's own drawn width did not shrink after halving "
          "pet->hunger_mp");

    /* The OTHER two bars, whose needs did not change, must not have moved
     * either -- proving this redraws exactly the bar that changed, not the
     * whole band on any change. */
    check(kf_creature_screen_debug_stat_bar_filled_px(1) == happiness_before,
          "the happiness bar's drawn width moved even though pet-"
          ">happiness_mp never changed");
    check(kf_creature_screen_debug_stat_bar_filled_px(2) == energy_before,
          "the energy bar's drawn width moved even though pet->energy_mp "
          "never changed");

    /* And the budget returns to steady once the change has been drawn:
     * a further frame with hunger_mp unchanged from what was JUST drawn
     * must cost nothing, proving this is "redraw on change", not "redraw
     * forever once touched". */
    kf_fb_clear_dirty();
    kf_creature_screen_frame(33u);
    const kf_dirty_rects settled = kf_fb_dirty_rects();
    check(settled.count <= 2,
          "the frame right after a stat-bar redraw still cost more than "
          "the creature's own <=2u budget -- the band should have gone "
          "quiet again immediately");

    KF_LOGI(TAG, "creature-screen-stats: hunger %d -> %d px after halving "
                 "pet->hunger_mp",
            hunger_before, hunger_after);

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Minor 6/7 of the pre-merge review (.superpowers/sdd/pre-merge-fixes-
 * report.md): every dirty-rect budget check above exercises at most two of
 * the three things that can move in the same frame, never all three
 * together, and none of them do it against the real creature pack.
 *
 * run_creature_screen_check()'s own <=2u bound covers the creature's
 * erase+draw and the mess row, but always against the checked-in DEFAULT
 * pack (examples/hello_sprite/assets.kfpack), which has no creature art --
 * every lookup this screen makes falls back to the placeholder rectangle,
 * never a real blit. run_creature_screen_stats_check() covers a bar
 * redraw, but calls kf_assets_init() with no pack override either (same
 * default, same placeholder), and only ever touches ONE need (hunger_mp)
 * at a time, never all three in the same frame. run_frame_counters_check()
 * is the one check that DOES mount KF_CREATURE_DEMO_PACK_PATH, but its own
 * dirty_rect_count bound is [1, KF_MAX_DIRTY_RECTS] -- 1 to 8 -- loose
 * enough that the animated path costing anywhere in that whole range would
 * not fail it.
 *
 * This drives the actual combination: the real, animated creature_demo
 * pack (child's real "neutral" 9-frame idle, checked below against a
 * placeholder to prove it -- not the pack's own single-frame egg), a
 * poop_count that changes every frame, and all three needs pushed across a
 * quantisation boundary every frame, together, while the creature is
 * genuinely wandering (real dt_ms -- not the dt_ms==0 the sprite-only
 * checks use to hold position and facing still). kf_pet_session_debug_
 * jump_to_stage() is what makes three bars moving at once a realistic
 * on-device event rather than a scenario invented for this test alone: it
 * calls kf_pet_init() internally, which snaps hunger_mp/happiness_mp/
 * energy_mp all back to KF_PET_MILLIPERCENT_MAX in the same call (see that
 * function's own comment in kf_pet_session.cpp) -- so a KFDBG JUMP off a
 * pet whose needs had decayed unevenly moves all three bars on the very
 * next frame a real device draws. The loop below does not rely on landing
 * on that one frame, though: it forces the same three-way change every
 * iteration of a run of frames and keeps the worst dirty-rect count seen,
 * the same "worst observed over many frames" shape every other budget
 * check in this file already uses -- because the wander's own step size
 * (hakoniwaos/src/creature.cpp's kSpeedPxPerSec) means whether the erase
 * and draw rectangles happen to touch (merging to one) or not (staying
 * two) varies frame to frame, not on a single hand-picked one. */
static int run_creature_screen_budget_combination_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-screen-budget-combo-" +
         std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    kf_arena_init_all();
    kf_fb_init();
    kf_host_assets_set_pack_path(KF_CREATURE_DEMO_PACK_PATH);
    check(kf_assets_init() == KF_OK,
          "kf_assets_init mounts examples/creature_demo/assets.kfpack");

    kf_pet_session_init();
    kf_creature_screen_init();

    /* Confirm the pack this check relies on actually carries the real,
     * animated art this file's own header comment claims -- not the
     * placeholder rectangle every OTHER dirty-rect budget check above
     * measures against. All three directions, because the wander below is
     * free to visit any of them. */
    for (const char *name :
         {"child_neutral_s", "child_neutral_e", "child_neutral_n"}) {
        const kf_sprite *s = kf_assets_get(name);
        check(s != nullptr && s->frame_count == 9u,
              "the creature_demo pack's child neutral idle is not the "
              "real 9-frame animated entry this check is supposed to be "
              "measuring against");
    }

    /* Not an egg -- the egg neither wanders (kf_creature_update() is never
     * called for it) nor animates through this screen's normal per-frame
     * path, so it could never actually reach the worst case this check is
     * looking for. kf_pet_session_debug_jump_to_stage()'s own "tops every
     * need back up" behaviour (this function's header comment) is why a
     * CHILD is realistic here even before the loop below starts forcing
     * per-frame changes of its own. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->stage = KF_PET_STAGE_CHILD;

    /* One untimed frame to let the screen notice the stage and the
     * uneven starting needs below, and start the creature wandering, none
     * of which the loop's own worst-case measurement should include. */
    pet->hunger_mp = 20000u;    /* 20% -- decayed unevenly, on purpose: */
    pet->happiness_mp = 55000u; /* three different quantised widths, so */
    pet->energy_mp = 90000u;    /* the first loop iteration's toggle */
                                 /* below is a real change for all three, */
                                 /* not a no-op that happens to already */
                                 /* match. */
    kf_creature_screen_frame(0u);

    size_t worst_rects = 0;
    bool ever_touched_band = false;
    bool ever_touched_field = false;
    for (int i = 0; i < 300; ++i) {
        /* Alternate both ends of the range every iteration, not just past
         * SOME quantisation boundary -- guarantees each bar's QUANTISED
         * width (update_stat_bar()'s own comment, kf_creature_screen.cpp)
         * differs from what was drawn last iteration, every iteration,
         * rather than leaving that to chance. Same reasoning for
         * poop_count: two different in-range counts, neither of which
         * ever repeats back to back. */
        const bool phase = (i % 2) == 0;
        pet->hunger_mp = phase ? 15000u : 85000u;
        pet->happiness_mp = phase ? 85000u : 15000u;
        pet->energy_mp = phase ? 30000u : 70000u;
        pet->poop_count = phase ? 3u : 7u;

        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > worst_rects) {
            worst_rects = static_cast<size_t>(d.count);
        }
        for (int r = 0; r < d.count; ++r) {
            if (d.rects[r].y1 > 260) { ever_touched_band = true; }
            if (d.rects[r].y0 < 260) { ever_touched_field = true; }
        }
    }

    check(ever_touched_band,
          "300 frames of forced need changes never dirtied the stats "
          "band (y>=260) -- the bars never actually redrew, so this "
          "check was not measuring what it claims to");
    check(ever_touched_field,
          "300 frames of forced poop_count changes never dirtied the "
          "field (y<260) -- the mess/creature drawing never actually "
          "redrew, so this check was not measuring what it claims to");

    /* The ANALYTICAL ceiling is 6: creature erase+draw (up to 2,
     * run_creature_screen_check()'s own bound) + mess (1, changed every
     * iteration above) + all three stat bars (up to 3, each changed every
     * iteration above). What this loop actually MEASURES, run after run,
     * seed after seed, is 5 -- the erase and draw rectangles merge into one
     * (kf/framebuffer.h's kf_fb_mark_dirty() comment on touching/
     * overlapping rects) on every single frame here, not just most of
     * them, because kSpeedPxPerSec (hakoniwaos/src/creature.cpp, 18px/sec)
     * moves the creature well under one pixel most 33ms ticks -- nowhere
     * near enough for two 48x48 rectangles one step apart to fail to
     * touch. Reaching the full 6 would need a DISCONTINUOUS reposition
     * (a life-stage jump's re-centre, or a death/revive) landing on the
     * exact same frame as this loop's forced mess+bars churn, which this
     * check's fixed-stage wander never produces -- a real gap, called out
     * here rather than papered over with a loose bound.
     *
     * Bounded at 5, the number actually measured, not at 6 or at
     * KF_MAX_DIRTY_RECTS (8) the way run_frame_counters_check()'s loose
     * check does: a bound of 6 would not fail if the creature's own
     * erase+draw regressed from merging to not merging (e.g. a change to
     * kSpeedPxPerSec, or to the merge threshold in touches_or_overlaps()),
     * which is exactly the kind of regression this check exists to catch.
     * If a future check drives that discontinuous-reposition case too and
     * legitimately needs 6, raise this bound then, with that check's own
     * measurement -- not preemptively, on the strength of arithmetic
     * this loop does not actually exercise. */
    check(worst_rects <= 5u,
          "the creature+mess+stats combination cost more than the 5 dirty "
          "rectangles this check has always measured as its worst case -- "
          "see this check's own comment on why 5, not the 6-rectangle "
          "analytical ceiling, is the right bound for what it drives");

    KF_LOGI(TAG,
            "creature-screen-budget-combo: worst frame %zu rects (bound 5, "
            "analytical ceiling 6)",
            worst_rects);

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Playback (Task 6, animated-indexed-sprites plan): the cursor advances on
 * its own ~10fps clock regardless of the display's, wraps at the end,
 * resets when the resolved sprite changes, and -- the thing that would
 * quietly wreck the frame budget if it were wrong -- costs no extra dirty
 * rectangles, because the screen already redrew the creature every frame
 * anyway (see kf_creature_screen_frame()'s own comment on the erase-then-
 * draw pattern this relies on).
 *
 * Isolates its own storage directory the same way every other check that
 * calls kf_pet_session_init() does (run_creature_screen_check() et al.):
 * kf_pet_load_and_advance() falls back to a fresh pet on a missing save, so
 * skipping this would not fail outright, but it would leave this check
 * reading and writing whatever relative "kf_save" directory happens to be
 * lying around from a previous ctest run in the same build directory --
 * exactly the shared-state flakiness every neighbouring check already
 * avoids. */
static int run_creature_anim_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-creature-anim-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    check(kf_store_init() == KF_OK, "kf_store_init");
    check(kf_power_init() == KF_OK, "kf_power_init");

    /* The clock, in isolation: pure Core, no screen, no assets. */
    kf_creature c;
    const kf_rect field = {0, 0, 240, 260};
    kf_rng_seed(1u);
    kf_creature_init(&c, field);
    check(c.anim.frame == 0u, "a fresh creature starts on frame 0");

    /* 100ms of animation at a 33ms display tick is 3 ticks with 1ms of
     * remainder carried, not 3 ticks with the remainder thrown away. */
    kf_creature_tick_anim(&c, 33u);
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 0u, "66ms is not yet a frame");
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 0u, "99ms is still not a frame, at 100ms each");
    kf_creature_tick_anim(&c, 33u);
    check(c.anim.frame == 1u, "the cursor advanced exactly once by 132ms");
    check(c.anim.accum_ms == 32u,
          "and kept the 32ms remainder rather than resetting to zero");

    /* Ten seconds at 33ms should be 100 frames' worth of advance, give or
     * take one tick's rounding -- the accumulator carrying its remainder is
     * what makes this true rather than 9.1fps drift. */
    kf_creature_init(&c, field);
    uint32_t advances = 0u;
    uint16_t last = c.anim.frame;
    for (int i = 0; i < 303; ++i) {
        kf_creature_tick_anim(&c, 33u);
        if (c.anim.frame != last) { ++advances; last = c.anim.frame; }
    }
    check(advances >= 99u && advances <= 100u,
          "10 seconds of 33ms ticks advances ~100 frames, not ~91 -- the "
          "accumulator is carrying its remainder");

    /* Wrapping. */
    kf_creature_init(&c, field);
    c.anim.frame = 2u;
    kf_creature_anim_wrap(&c, 3u);
    check(c.anim.frame == 2u, "an in-range cursor is left alone");
    c.anim.frame = 7u;
    kf_creature_anim_wrap(&c, 3u);
    check(c.anim.frame == 0u,
          "a cursor past the end of a shorter animation resets to 0 rather "
          "than wrapping to a frame the previous pose happened to be on");

    /* On screen, against the real animated fixture, and against the budget.
     * The pack has no egg/pose art at all (it only carries test_sprite and
     * test_sprite_anim), so every frame draws the placeholder rectangle --
     * that is fine, this loop is proving the BUDGET holds while animation
     * is wired in, not proving the fixture's art gets drawn (the block
     * below, against the real sprite pointer, proves that instead). */
    kf_arena_init_all();
    kf_fb_init();
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture mounts");
    kf_pet_session_init();
    kf_creature_screen_init();

    size_t worst_rects = 0;
    for (int i = 0; i < 300; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (static_cast<size_t>(d.count) > worst_rects) {
            worst_rects = static_cast<size_t>(d.count);
        }
    }
    check(worst_rects <= 2u,
          "animating the creature costs no extra dirty rectangles -- the "
          "screen already erased and redrew it every frame");

    /* End to end, against the real fixture rather than a synthetic frame
     * count: the format supports nine frames, but nothing in the repo has
     * shipped animated art yet (see this task's own report), so the only
     * real multi-frame entry anywhere is the fixture's test_sprite_anim.
     * Drives the same kf_creature_tick_anim()/kf_creature_anim_wrap() pair
     * the screen uses, directly against its real frame_count and its real
     * index bytes, so this fails if the cursor and the fixture ever
     * disagree about how many frames there are -- not just if the wrap
     * logic agrees with itself against a made-up number. */
    const kf_sprite *anim = kf_assets_get("test_sprite_anim");
    check(anim != nullptr && anim->frame_count == 3u,
          "the fixture's animated sprite is still a real 3-frame entry");
    if (anim != nullptr) {
        kf_creature anim_c;
        kf_creature_init(&anim_c, field);
        const size_t stride =
            static_cast<size_t>(anim->width) * anim->height;
        uint16_t visited[4] = {};
        for (int i = 0; i < 4; ++i) {
            kf_creature_anim_wrap(&anim_c, anim->frame_count);
            visited[i] = anim_c.anim.frame;
            kf_creature_tick_anim(&anim_c, KF_ANIM_FRAME_MS);
        }
        check(visited[0] == 0u && visited[1] == 1u && visited[2] == 2u &&
                  visited[3] == 0u,
              "the cursor walks all three of the fixture's real frames in "
              "order, then wraps back to the first");
        check(std::memcmp(anim->indices + visited[0] * stride,
                           anim->indices + visited[1] * stride, stride) != 0,
              "frame 0 and frame 1 of the fixture are genuinely different "
              "pixel content, not the same picture drawn three times");
        check(std::memcmp(anim->indices + visited[1] * stride,
                           anim->indices + visited[2] * stride, stride) != 0,
              "frame 1 and frame 2 of the fixture are genuinely different "
              "pixel content too");
    }

    kf_pet_session_shutdown();
    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);
    kf_power_shutdown();
    kf_store_shutdown();
    std::filesystem::remove_all(dir, rm_ec);

    if (ok) {
        KF_LOGI(TAG,
                "creature-anim: cursor keeps its own clock, budget unmoved");
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Task 1 of the hardware bring-up plan (docs/superpowers/plans/
 * 2026-08-11-hardware-bringup.md): proves the frame budget counters are not
 * structurally zero on the KF_DEMO_NONE path a real device runs.
 *
 * Before hakoniwaos/src/app.cpp moved kf_draw_counters_reset() from the top
 * of kf_app_frame() to immediately after kf_draw_counters_get(), this would
 * have failed forever, not by accident: on KF_DEMO_NONE, kf_demo_update()/
 * kf_demo_draw() both return immediately (hakoniwaos/src/demo.cpp), so
 * nothing draws inside kf_app_frame() at all -- the creature is drawn by a
 * PORT after kf_app_frame() returns, exactly the way app_main.cpp:247-290
 * drives it: kf_app_frame(), THEN kf_screen_nav_frame() (which routes to
 * the creature screen when it is the active one). This check drives the
 * creature screen directly through kf_creature_screen_frame() -- the same
 * entry point run_creature_screen_check() above already uses to advance it
 * -- rather than through kf_screen_nav_frame(), which would also require
 * standing up LVGL (kf_lvgl_port_init()/kf_screen_nav_init()) for nothing
 * this check needs: the point is the ORDER (kf_app_frame() returns, THEN
 * something draws), not the routing that gets there on a real device.
 *
 * Needs the real creature_demo pack (KF_CREATURE_DEMO_PACK_PATH), not the
 * default pack run_creature_screen_check() mounts: only a real, colour-
 * keyed sprite blit posts to the KEYED bucket kf/blit.h describes. The
 * placeholder kf_fill_rect() the default pack falls back to (no creature
 * art in it) posts to the OPAQUE bucket instead -- which would make
 * keyed_pixels legitimately 0 for a reason that has nothing to do with the
 * bug this check exists to catch, exactly the vacuous-pass trap this
 * task's brief warns about (see also 2026-08-09-creature-on-screen.md). */
static int run_frame_counters_check(void) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-frame-counters-" + std::to_string(KF_GETPID()));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
    kf_host_storage_set_dir(dir.string().c_str());

    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_host_assets_set_pack_path(KF_CREATURE_DEMO_PACK_PATH);
    kf_app_init(KF_DEMO_NONE);
    kf_pet_session_init();
    kf_creature_screen_init();

    /* Task 4 of the Lua game-layer plan (docs/superpowers/plans/
     * 2026-08-12-lua-game-layer.md): the creature screen is now retained --
     * kf_creature_screen_frame() declares the creature to kf/scene.h and
     * kf_scene_commit() paints only what actually changed since last frame
     * (kf/scene.h's own "A frame in which nothing was changed produces zero
     * dirty rectangles and draws nothing" comment). A fresh pet is an EGG
     * (kf_pet_init(), kf/pet.h), which does not wander -- it only bobs by
     * egg_bob_offset_y()'s own wave, which is exactly 0 for the first three
     * 33ms ticks this loop drives (kEggBobQuarterMs is 750ms). Left as an
     * egg, every one of these frames would genuinely declare the SAME
     * position and sprite as the frame before it, kf_scene_commit() would
     * correctly draw nothing, and keyed_pixels would read 0 for a true
     * reason having nothing to do with the defect this check exists to
     * catch -- exactly the "check passes because the drawing was deleted"
     * trap this file's own banner and 2026-08-09-creature-on-screen.md warn
     * about, this time sprung by a genuinely-idle frame rather than a
     * genuinely-missing draw call. Forcing CHILD here (the same lever
     * run_creature_screen_check() already uses) makes every one of the
     * frames below a real wander tick, so the creature's declared position
     * differs from the last-presented one on every single frame and a real
     * kf_blit_frame() keyed draw happens for real -- not because the bound
     * below was loosened to let a silent frame through. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->stage = KF_PET_STAGE_CHILD;

    /* At least 3 iterations, not 1: the counters' window is now one frame
     * BEHIND the draw that fills it -- kf_app_frame() reads and resets the
     * PREVIOUS iteration's counters before this iteration's kf_creature_
     * screen_frame() call below has drawn anything new (see app.cpp's
     * kf_draw_counters_reset() comment for exactly why). A 1-iteration
     * loop would read a window nothing had drawn into yet and prove
     * nothing about the lag; 3 exercises the steady state past that first
     * edge, the same reasoning this task's brief gives.
     *
     * dt_ms=1000, not the usual 33: what kf_app_last_frame() reports after
     * this loop is the SECOND-TO-LAST kf_creature_screen_frame() call's
     * draw (index 1 of 0..2 -- the lag above means the last call's own
     * draw is never read by anyone). A fresh kf_creature starts on a fixed
     * kDwellMinMs=400ms dwell before its wander ever takes its first step
     * (hakoniwaos/src/creature.cpp), and kf_creature_update() returns
     * without moving on the very tick that dwell reaches 0, not just while
     * it is still positive -- so a real 33ms tick would need roughly
     * ceil(400/33)=13 of them just to clear the dwell, and this loop only
     * runs 3. 1000ms clears the whole dwell on iteration 0 (and starts
     * animating, but does not move -- the same "returns on the tick dwell
     * hits zero" rule) and then genuinely walks the creature ~18px on
     * iteration 1, so iteration 1's draw -- the one this check actually
     * reads -- contains a real, non-vacuous kf_blit_frame() of the CHILD
     * stage's real, colour-keyed sprite, not a frame where nothing changed
     * and kf_scene_commit() correctly drew nothing. See this function's own
     * header comment on why relying on the CHILD sprite alone (set once,
     * before this loop) is not enough by itself: that produces exactly one
     * real draw, at iteration 0, and the lag above means iteration 0's own
     * draw is read into kf_app_last_frame() by iteration 1's kf_app_frame()
     * call -- then immediately overwritten, still holding that value, by
     * iteration 2's kf_app_frame() call reading iteration 1's draw, which
     * without real movement would be genuinely empty. */
    for (int i = 0; i < 3; ++i) {
        check(kf_app_frame(), "kf_app_frame returned false");
        kf_creature_screen_frame(1000u);
    }

    const kf_frame_stats *last = kf_app_last_frame();
    check(last->keyed_pixels > 0,
          "keyed_pixels is 0 -- the counters' window does not overlap the "
          "creature's draw. This is the exact defect this check exists to "
          "catch: it would stay 0 even if the indexed blit were a hundred "
          "times slower than assumed.");
    check(last->keyed_pixels >= 2000 && last->keyed_pixels <= 8000,
          "keyed_pixels is outside the expected order of magnitude for a "
          "single 48x48 creature sprite draw -- a window, not an exact "
          "figure, because pose and mess legitimately move the exact "
          "count from frame to frame");
    check(last->dirty_rect_count >= 1 &&
              last->dirty_rect_count <= KF_MAX_DIRTY_RECTS,
          "dirty_rect_count is outside [1, KF_MAX_DIRTY_RECTS]");

    KF_LOGI(TAG,
            "frame-counters: keyed_pixels=%u opaque_pixels=%u "
            "dirty_rect_count=%u",
            last->keyed_pixels, last->opaque_pixels, last->dirty_rect_count);

    kf_pet_session_shutdown();
    kf_app_shutdown();
    kf_host_assets_set_pack_path(nullptr);
    std::filesystem::remove_all(dir, rm_ec);

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

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

/* Proves the menu/screen navigation mechanism (ADR 0022): Home is active
 * immediately after kf_screen_nav_init(), kf_screen_nav_debug_advance()
 * (the same effect a real MENU press has) moves to Info and Info renders
 * deterministically, and kf_screen_nav_debug_home() (the same effect a
 * real B press has) returns to Home -- each step asserted against
 * kf_screen_nav_debug_index() directly, not inferred from the checksum
 * alone. Uses the debug/test entry points rather than a scripted button
 * sequence through kf_app_frame(), because headless_input.cpp's
 * kf_headless_script() is one fixed, frame-indexed script shared by every
 * check that drives kf_app_frame() (headless_determinism among them);
 * teaching it a MENU window would change what those unrelated,
 * already-locked golden checksums see too. See kf_screen_nav.h's own
 * header comment on why these entry points exist at all.
 *
 * Same isolated-per-PID storage directory trick as run_pet_screen_check()
 * above, for the same reason: kf_pet_session_init() needs somewhere to
 * read/write that will not collide with another test running at the same
 * time. */
int run_screen_nav_check(unsigned long long expect_checksum,
                          bool have_expect) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("kamiframe-headless-screen-nav-" + std::to_string(KF_GETPID()));
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

    kf_arena_init_all();
    kf_fb_init();

    kf_pet_session_init();
    kf_lvgl_port_init();
    kf_screen_nav_init();

    check(kf_screen_nav_debug_index() == 0,
          "Home is active immediately after kf_screen_nav_init()");

    /* Same synthetic, fixed per-frame clock as run_pet_screen_check()
     * above, for the same reason -- real time between iterations would be
     * scheduler noise, not frame count. kf_screen_nav_frame() runs every
     * iteration the same way the interactive build's loop does, even
     * though nothing here presses a real button -- proving the per-frame
     * "call the active screen's update()" path, not just the switch
     * itself. kf_lvgl_port_pump() is guarded by kf_screen_nav_wants_lvgl(),
     * the same as sdl_main.cpp/app_main.cpp: Home is the creature screen
     * now (Task 4) and draws straight into the framebuffer, so pumping
     * LVGL while it is active would run lv_timer_handler() over whatever
     * LVGL's own default screen happens to be -- still invalidated from
     * lv_init(), since nothing has ever loaded a real LVGL screen yet at
     * this point -- and its flush would overwrite every pixel the creature
     * screen just drew, defeating this whole loop. */
    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);
    for (int i = 0; i < 30; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_screen_nav_frame(kFixedDtMs);
        if (kf_screen_nav_wants_lvgl()) {
            kf_lvgl_port_pump(kFixedDtMs);
        }
    }
    check(kf_screen_nav_debug_index() == 0,
          "still on Home after 30 quiet frames (nothing should have "
          "switched screens on its own)");

    /* Home, exactly as the device draws it -- this is the same init order and
     * the same per-frame order app_main.cpp uses. */
    dump_framebuffer_ppm(g_dump_path);

    kf_screen_nav_debug_advance();
    check(kf_screen_nav_debug_index() == 1,
          "kf_screen_nav_debug_advance() moved from Home to Info");

    for (int i = 0; i < 30; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_screen_nav_frame(kFixedDtMs);
        if (kf_screen_nav_wants_lvgl()) {
            kf_lvgl_port_pump(kFixedDtMs);
        }
    }

    /* Checksummed with Info on screen -- the same FNV-1a-over-the-
     * framebuffer approach every other rendering check in this codebase
     * uses, for the same reason: this is fundamentally "does this set of
     * widget calls draw the same pixels every time", not an exact-
     * arithmetic invariant. */
    uint64_t checksum = 1469598103934665603ull;
    const uint8_t *bytes =
        reinterpret_cast<const uint8_t *>(kf_fb_pixels());
    for (size_t i = 0; i < KF_FRAMEBUFFER_BYTES; ++i) {
        checksum ^= bytes[i];
        checksum *= 1099511628211ull;
    }

    kf_screen_nav_debug_home();
    check(kf_screen_nav_debug_index() == 0,
          "kf_screen_nav_debug_home() returned from Info to Home");

    /* Controller amendment A7.1 (a Task 4 review fix that nothing else
     * catches if reverted): kf_creature_screen_enter() -- which the
     * kf_screen_nav_debug_home() call just above ran, via load()'s switch-
     * to-Home path -- must repaint the WHOLE 240x320 panel, not just
     * kField (y=[0,260)). Info is an LVGL screen and never touches rows
     * 260-319 itself, so if Home's entry repaint only covered its own
     * field, whatever Info last flushed into that band -- from before this
     * very round trip -- would sit there forever, since Home never pumps
     * LVGL to repaint it and no per-frame path here would ever touch it
     * either. The golden checksum above does not cover this: it is taken
     * while INFO is active, before this Home switch even happens.
     *
     * No new sprite/creature drawing has run yet at this exact point --
     * kf_creature_screen_frame() has not been called since re-entering --
     * so EVERY pixel on the panel is still exactly whatever
     * kf_creature_screen_enter()'s fill just painted -- kBackground
     * (kf_creature_screen.cpp), reproduced here by value rather than
     * sampled from a second live pixel: sampling (0,0) instead would catch
     * a REGRESSED entry repaint (kField-only, band pixel still Info's)
     * exactly as well, but would pass vacuously if kf_creature_screen_
     * enter()'s fill were removed altogether -- both samples would then
     * still hold whatever Info left, agree with each other, and this check
     * would report success while nothing had actually repainted anything.
     * Comparing against the known constant instead catches that case too:
     * with no fill at all, the band pixel is Info's leftover colour, which
     * this fixed value was never going to equal by chance. */
    {
        const kf_color *px = kf_fb_pixels();
        const kf_color kKnownCreatureBackground = KF_RGB(232, 240, 216);
        const kf_color band_pixel =
            px[static_cast<size_t>(280) * KF_DISPLAY_WIDTH + 10u];
        check(band_pixel == kKnownCreatureBackground,
              "row 280 (inside the reserved y=[260,320) stats band) still "
              "held Info's leftover pixels after switching back to Home -- "
              "kf_creature_screen_enter() must repaint the whole panel, "
              "not just kField");
    }

    kf_screen_nav_debug_home();
    check(kf_screen_nav_debug_index() == 0,
          "kf_screen_nav_debug_home() is a no-op when already on Home");

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
                "Info screen changed. If that was deliberate, update "
                "KAMIFRAME_SCREEN_NAV_GOLDEN_CHECKSUM in "
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
    constexpr long kStage2Frames = 30;

    /* Stage 1: decay-and-read. No care calls at all -- just enough live
     * frames for hunger to visibly move.
     *
     * A fresh pet session starts in KF_PET_STAGE_EGG (ADR 0021), which
     * deliberately does not decay -- so this stage first has to clear the
     * default config's egg_duration_seconds (3600s = 1 hour, see
     * kf_pet_default_config()) before hunger moves at all. kStage1DtMs is
     * a larger-than-real-frame-time synthetic dt for exactly that reason:
     * kf_pet_session_frame() does not care whether the dt it is handed is
     * a plausible single frame's worth or not, only that dt accumulates
     * correctly (see that function's own header comment on why it
     * batches at all). At kStage1DtMs=3500ms/frame, kStage1Frames=1200
     * frames sums to 4,200,000ms = 4200 simulated seconds -- clearing the
     * 3600s egg stage with 600s (10 minutes) left over inside the baby
     * stage, comfortably enough for kf_pet_default_config()'s ~1042
     * mp/hour hunger decay rate to produce a visible, comfortably nonzero
     * drop (about 174 mp) without coming close to exhausting the need
     * entirely. This is still a binding-correctness check, not a
     * stage-timing one -- see this file's own header comment on what
     * these two proof scripts exist to prove. */
    constexpr uint32_t kStage1DtMs = 3500u;
    constexpr long kStage1Frames = 1200;
    check(kf_lua_port_init(kKfLuaPetDecayProofScriptSource,
                            kKfLuaPetDecayProofScriptChunkName),
          "stage 1 (decay) proof script loaded");
    for (long i = 0; i < kStage1Frames; ++i) {
        kf_pet_session_frame(kStage1DtMs);
        kf_lua_port_frame(kStage1DtMs);
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

    /* Stage 3: stage/evolution reads (ADR 0021), continuing the SAME
     * session once again. Stage 1 already accumulated 4200 simulated
     * seconds, clearing the default config's 3600s egg duration -- so by
     * now the live session is in KF_PET_STAGE_BABY, not the egg it started
     * as, which is what makes this a real check of pet.stage() rather than
     * a tautological "still egg" one. teen_form/adult_branch are still 0
     * (Baby is well before either branch point). The C++ side packs the
     * expected value with the exact same stage*1000 + teen_form*10 +
     * adult_branch encoding the script uses, from the live state read
     * directly -- not from any assumption about what stage "should" be by
     * now. */
    check(kf_lua_port_init(kKfLuaPetStageProofScriptSource,
                            kKfLuaPetStageProofScriptChunkName),
          "stage 3 (stage/evolution) proof script loaded");
    for (long i = 0; i < kStage2Frames; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_lua_port_frame(kFixedDtMs);
    }
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kStage2Frames),
          "stage 3 ran the full frame count without a script error");
    const kf_pet_state *live_after_stage = kf_pet_session_state();
    check(live_after_stage->stage == KF_PET_STAGE_BABY,
          "the live session has progressed past the egg stage by stage 3, "
          "which is what makes the report comparison below a real check "
          "rather than a trivially-true one");
    const int64_t expected_packed =
        static_cast<int64_t>(live_after_stage->stage) * 100000 +
        static_cast<int64_t>(live_after_stage->teen_form) * 10000 +
        static_cast<int64_t>(live_after_stage->adult_branch) * 1000 +
        static_cast<int64_t>(live_after_stage->base_trait) * 10 +
        static_cast<int64_t>(kf_pet_dominant_care_trait(live_after_stage));
    check(kf_lua_port_last_report() == expected_packed,
          "pet.stage()/pet.teen_form()/pet.adult_branch()/pet.base_trait()/"
          "pet.dominant_care_trait() (ADR 0023) read via Lua, packed into "
          "one integer, match the identical packing computed directly "
          "from kf_pet_session_state()/kf_pet_dominant_care_trait() in "
          "C++ -- proves the string pet.stage() hands back round-trips to "
          "the correct enum value, not just that some string comes out, "
          "and that the two new personality accessors are wired to the "
          "same live state as everything else here");
    kf_lua_port_shutdown();

    /* Stage 4: mess (the care-loop spec's section 4), still the same
     * session. Mess is the one care type with no bar of its own, so the
     * only way a script can react to it is through pet.poops() and
     * pet.dirtiness() -- and the only way a player can act on it is
     * pet.bath() and pet.flush(). Both halves are checked here.
     *
     * The dt is large on purpose: poops arrive on a 1800-second timer
     * (kf_pet_default_config()), so proving the reads see something other
     * than a constant zero needs simulated hours, not frames. 60 frames of
     * 120000ms is 7200 simulated seconds -- four poop intervals, enough
     * that the count is unambiguously nonzero without relying on the exact
     * interval. */
    constexpr uint32_t kStage4DtMs = 120000u;
    constexpr long kStage4Frames = 60;
    check(kf_lua_port_init(kKfLuaPetMessProofScriptSource,
                            kKfLuaPetMessProofScriptChunkName),
          "stage 4 (mess) proof script loaded");
    for (long i = 0; i < kStage4Frames; ++i) {
        kf_pet_session_frame(kStage4DtMs);
        kf_lua_port_frame(kStage4DtMs);
    }
    const kf_pet_state *live_messy = kf_pet_session_state();
    check(live_messy->poop_count > 0u && live_messy->dirtiness_mp > 0u,
          "the live session really did get messy over stage 4 -- without "
          "this the report comparison below would pass on two zeroes");
    check(kf_lua_port_last_report() ==
              static_cast<int64_t>(live_messy->poop_count) * 1000000 +
                  static_cast<int64_t>(live_messy->dirtiness_mp),
          "pet.poops() and pet.dirtiness() read via Lua match the live "
          "C++ state exactly");
    kf_lua_port_shutdown();

    /* Now clean it up from Lua. dt is zero for these frames so no new mess
     * can arrive between the call and the check -- what is being proven is
     * that the two calls reached Core, not how fast mess returns. */
    check(kf_lua_port_init(kKfLuaPetCleanProofScriptSource,
                            kKfLuaPetCleanProofScriptChunkName),
          "stage 4 (clean) proof script loaded");
    kf_pet_session_frame(0u);
    kf_lua_port_frame(0u);
    const kf_pet_state *live_cleaned = kf_pet_session_state();
    check(kf_lua_port_last_report() == 0,
          "pet.poops() reports zero straight after pet.flush()");
    check(live_cleaned->poop_count == 0u && live_cleaned->dirtiness_mp == 0u,
          "pet.bath() and pet.flush() called from Lua cleared both halves "
          "of mess in the "
          "live C++ state -- checked directly, independent of what the "
          "script itself reported");
    kf_lua_port_shutdown();

    /* Stage 5: illness. The session has been fed by the care stage above,
     * which matters -- an untouched creature is exempt from the accumulator
     * entirely, so without that earlier care this stage would prove nothing
     * but that two zeroes match. It is asserted rather than assumed.
     *
     * The creature was just cleaned, so this has to drive it back down: no
     * care at all, and hours of it. 120 frames of 300000ms is 36000
     * simulated seconds -- ten hours, well past the three-hour onset even
     * after the first stretch nets zero. */
    constexpr uint32_t kStage5DtMs = 300000u;
    constexpr long kStage5Frames = 120;
    check(kf_pet_session_state()->care_actions_taken > 0u,
          "the session has been cared for at least once by now, which is "
          "what makes it eligible to fall ill at all");
    check(kf_lua_port_init(kKfLuaPetHealthProofScriptSource,
                            kKfLuaPetHealthProofScriptChunkName),
          "stage 5 (health) proof script loaded");
    for (long i = 0; i < kStage5Frames; ++i) {
        kf_pet_session_frame(kStage5DtMs);
        kf_lua_port_frame(kStage5DtMs);
    }
    const kf_pet_state *live_ill = kf_pet_session_state();
    check(live_ill->sick && live_ill->neglect_seconds > 0u,
          "the live session really did fall ill over stage 5 -- without "
          "this the comparison below would pass on zeroes");
    const int64_t expected_health =
        (live_ill->sick ? 1 : 0) * 10000000 + (live_ill->dead ? 2 : 0) * 10000000 +
        static_cast<int64_t>(live_ill->neglect_seconds);
    check(kf_lua_port_last_report() == expected_health,
          "pet.sick(), pet.dead() and pet.neglect_seconds() read via Lua "
          "match the live C++ state exactly");
    kf_lua_port_shutdown();

    /* Stage 6: care variations (docs/superpowers/plans/2026-08-09-care-
     * variations.md), still the same session. The script itself works out
     * which variation of feeding its own (randomly rolled) pet.base_trait()
     * is DISLIKED via pet.reaction_to() -- proving that binding too, rather
     * than assuming C++ and Lua agree on it -- then feeds with exactly that
     * variation and reports pet.last_reaction()/pet.last_care_action()
     * packed into one integer. The preference table's own exactly-one-of-
     * each invariant (run_pet_preferences_check()) guarantees a disliked
     * variation exists for every possible base_trait, so the reaction is
     * known to land on DISLIKED specifically, not just "some reaction" --
     * asserted directly against the live C++ state before comparing, so
     * this cannot pass on two neutral zeroes the way the mess and health
     * stages above guard against. */
    check(kf_lua_port_init(kKfLuaPetReactionProofScriptSource,
                            kKfLuaPetReactionProofScriptChunkName),
          "stage 6 (reaction) proof script loaded");
    kf_pet_session_frame(0u);
    kf_lua_port_frame(0u);
    const kf_pet_state *live_reacted = kf_pet_session_state();
    check(live_reacted->last_reaction == KF_PET_REACTION_DISLIKED,
          "feeding with the variation this trait dislikes really did record "
          "DISLIKED -- without this the comparison below could pass on two "
          "zeroes");
    const int64_t expected_reaction =
        static_cast<int64_t>(live_reacted->last_reaction) * 10 +
        static_cast<int64_t>(live_reacted->last_care_action);
    check(kf_lua_port_last_report() == expected_reaction,
          "pet.last_reaction() and pet.last_care_action() read via Lua "
          "match the live C++ state exactly, and pet.reaction_to() (used by "
          "the script itself to find the disliked variation) agrees with "
          "kf_pet_reaction_to() in C++");
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

    /* Stage 1: decay from full to empty. An hour of elapsed pet-time per
     * call, applied in one kf_pet_advance() call each (kf_pet_session_
     * frame() batches whatever it's handed, see its own header comment --
     * a single large dt is not a special case for it). Sixteen hours
     * comfortably exhausts even the slowest of the three configured decay
     * rates (see kf_pet_default_config(), ADR 0015): every need should be
     * clamped at exactly zero by the end, which the check below confirms
     * directly against the live C++ state -- not because the demo creature
     * script needs that number, but so stage 2's "recovered to full"
     * transition below is known to start from genuine rock bottom, not
     * merely "low".
     *
     * SIXTEEN HOURS, NOT TEN DAYS, and the difference matters. Ten days of
     * total neglect kills the creature outright now, and a dead creature
     * ignores every care action -- so stage 2 would have quietly stopped
     * testing anything at all while still reading like it passed. Sixteen
     * hours drains every need to nothing while staying well inside the
     * twenty-four hours of accumulated neglect that would be fatal. */
    constexpr uint32_t kOneHourMs = 60u * 60u * 1000u;
    constexpr long kStage1Frames = 16;
    for (long i = 0; i < kStage1Frames; ++i) {
        kf_pet_session_frame(kOneHourMs);
        kf_lua_port_frame(kOneHourMs);
    }
    check(kf_lua_port_frame_count() == static_cast<uint32_t>(kStage1Frames),
          "stage 1 (decay through every band) ran the full frame count "
          "without a script error");
    const kf_pet_state *after_decay = kf_pet_session_state();
    check(after_decay->hunger_mp == 0u && after_decay->happiness_mp == 0u &&
              after_decay->energy_mp == 0u,
          "sixteen hours of elapsed time clamps every need to exactly "
          "zero -- confirms stage 2 below starts from genuine rock bottom");

    /* Stage 2: care back up to full, directly in C++ (not via the script --
     * this demo creature only reads, see kf_lua_demo_creature_script.h),
     * then a few more frames so on_frame() observes and announces the
     * "full" transition too, in the other direction from stage 1. Each
     * care action's boost now depends on how this session's (randomly
     * rolled) base_trait feels about variation 0 of that action (see
     * kf/pet.cpp's care_boost_liked_mp/neutral_mp/disliked_mp and the
     * care-variations plan). How many calls that takes therefore depends on
     * a trait rolled at random, so the count is DERIVED from the smallest
     * boost the config allows rather than written in: enough calls that
     * even a creature that dislikes variation 0 of everything reaches max.
     *
     * Deriving it matters more than it looks. The obvious version -- work
     * out that ten calls of the disliked boost happen to land exactly on
     * KF_PET_MILLIPERCENT_MAX and hard-code ten -- is correct today and
     * silently wrong the first time anyone tunes care_boost_disliked_mp
     * down, which is a number explicitly marked as being for living with.
     * Extra calls are free: the care actions clamp at max. */
    const kf_pet_config care_config = kf_pet_default_config();
    const kf_pet_millipercent smallest_boost =
        care_config.care_boost_disliked_mp > 0u
            ? care_config.care_boost_disliked_mp
            : KF_PET_MILLIPERCENT_MAX;
    const int calls_to_fill = static_cast<int>(
        (KF_PET_MILLIPERCENT_MAX + smallest_boost - 1u) / smallest_boost);
    for (int i = 0; i < calls_to_fill; ++i) {
        kf_pet_session_feed(0u);
        kf_pet_session_play(0u);
        kf_pet_session_rest(0u);
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

/* Proves the asset pipeline (ADR 0033) end to end: kf_assets_init() mounts
 * and parses the checked-in pack (examples/hello_sprite/assets.kfpack, the
 * same file kf_demo_init() loads "test_sprite" from -- ASSET_TYPE_SPRITE_
 * INDEXED since the animated-indexed-sprites plan's Task 3), kf_assets_get()
 * finds it with the size, frame count and colour key tools/kf_pack_assets.py
 * --test-sprite --indexed always writes, and -- the actual "matches what the
 * packer wrote" proof -- expanding every index through the pack FILE's own
 * palette bytes reproduces exactly the colour the loaded sprite yields, at
 * the offset ITS OWN directory entry names, read directly here with a small,
 * independent parse that does not go through kf/assets.cpp at all. That
 * last part matters: without it, this check would only prove kf/assets.cpp
 * agrees with itself, not that it read the file correctly.
 *
 * Bypasses kf_app_init(): only KF_ARENA_ASSETS needs to exist first, not a
 * display, storage, or the pet session -- the same "bring up only what this
 * check needs" pattern run_lvgl_check() and run_lua_check() already use. */
int run_asset_check() {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    kf_arena_init_all();

    check(kf_assets_init() == KF_OK, "kf_assets_init mounts and parses the "
                                      "checked-in pack");

    check(kf_assets_get("no_such_sprite") == nullptr,
          "a name absent from the pack returns NULL rather than a stale or "
          "garbage pointer");

    const kf_sprite *sprite = kf_assets_get("test_sprite");
    check(sprite != nullptr,
          "kf_assets_get(\"test_sprite\") finds the packed sprite by name");

    if (sprite != nullptr) {
        check(sprite->format == KF_SPRITE_FORMAT_INDEXED8,
              "the checked-in default pack is now indexed -- see "
              "docs/superpowers/plans/2026-08-10-animated-indexed-sprites.md");
        check(sprite->width == 32u && sprite->height == 32u,
              "test_sprite is 32x32, the fixed size "
              "tools/kf_pack_assets.py --test-sprite always writes");
        check(sprite->frame_count == 1u, "test_sprite is a single frame");
        check(sprite->has_color_key, "test_sprite carries a color key");
        check(sprite->indices[0] == KF_SPRITE_KEY_INDEX,
              "the sprite's own corner pixel (outside the body ellipse in "
              "the generator's math) reads back as the colour-key INDEX -- a "
              "real decoded pixel, not zeroed or garbage memory");
        check(sprite->palette[KF_SPRITE_KEY_INDEX] == sprite->color_key,
              "palette entry 0 is the colour key, the convention "
              "KF_SPRITE_KEY_INDEX names");

        /* Independently re-open and parse the pack FILE, byte for byte,
         * without reusing kf/assets.cpp's reader at all. */
        std::FILE *f = std::fopen(KF_HOST_DEFAULT_ASSET_PACK_PATH, "rb");
        check(f != nullptr, "the pack file kf_assets_init() just mounted "
                             "can be reopened directly for comparison");
        if (f != nullptr) {
            std::fseek(f, 0, SEEK_END);
            const long file_size = std::ftell(f);
            std::vector<uint8_t> raw(file_size > 0 ? static_cast<size_t>(file_size) : 0u);
            std::fseek(f, 0, SEEK_SET);
            const size_t read =
                raw.empty() ? 0u : std::fread(raw.data(), 1, raw.size(), f);
            std::fclose(f);
            check(read == raw.size(), "the pack file reads back in full");

            check(raw.size() >= 16u &&
                      std::memcmp(raw.data(), "KFAP", 4) == 0,
                  "the pack file on disk starts with the KFAP magic");

            uint16_t entry_count = 0u;
            uint32_t directory_offset = 0u;
            if (raw.size() >= 16u) {
                std::memcpy(&entry_count, raw.data() + 6, sizeof(entry_count));
                std::memcpy(&directory_offset, raw.data() + 8,
                            sizeof(directory_offset));
            }
            check(entry_count == 1u,
                  "the checked-in pack has exactly the one sprite this "
                  "check expects -- if that changed on purpose, this check "
                  "needs updating too");

            /* 52-byte entry: name(32) asset_type(1) reserved(1) reserved(2)
             * type_meta(8) data_offset(4) data_bytes(4) -- see
             * tools/kf_pack_assets.py's format comment. Read directly here
             * rather than via a shared struct/constant with
             * hakoniwaos/src/assets.cpp on purpose: this check exists to
             * catch the two ever drifting apart, so it must not share the
             * one piece of code that could hide that drift. */
            if (entry_count == 1u && directory_offset + 52u <= raw.size()) {
                const uint8_t *entry = raw.data() + directory_offset;
                char name[33] = {};
                std::memcpy(name, entry, 32);
                check(std::strcmp(name, "test_sprite") == 0,
                      "the pack file's own directory names this entry "
                      "'test_sprite'");

                const uint8_t asset_type = entry[32];
                check(asset_type == 2u,
                      "the pack file's own directory marks this entry as "
                      "ASSET_TYPE_SPRITE_INDEXED (2)");

                uint16_t width = 0u, height = 0u, frame_count = 0u;
                std::memcpy(&width, entry + 36, sizeof(width));
                std::memcpy(&height, entry + 38, sizeof(height));
                std::memcpy(&frame_count, entry + 40, sizeof(frame_count));
                const uint16_t palette_count =
                    static_cast<uint16_t>(entry[42]) + 1u;
                const bool has_color_key = (entry[43] & 0x01u) != 0u;

                check(width == sprite->width && height == sprite->height &&
                          frame_count == sprite->frame_count &&
                          palette_count == sprite->palette_count &&
                          has_color_key == sprite->has_color_key,
                      "the metadata in the file's own type_meta matches what "
                      "kf_assets_get() reported");

                uint32_t data_offset = 0u, data_bytes = 0u;
                std::memcpy(&data_offset, entry + 44, sizeof(data_offset));
                std::memcpy(&data_bytes, entry + 48, sizeof(data_bytes));

                check(static_cast<uint64_t>(data_offset) + data_bytes <=
                          raw.size(),
                      "the payload the directory names actually fits inside "
                      "the file");

                const uint32_t palette_padded =
                    ((static_cast<uint32_t>(palette_count) * 2u) + 3u) & ~3u;
                check(data_bytes == palette_padded +
                                        static_cast<uint32_t>(frame_count) *
                                            width * height,
                      "data_bytes recorded in the file's own directory "
                      "matches padded palette + frames*w*h");

                /* The real "matches what the packer wrote" proof, now that a
                 * pixel is two file reads instead of one: expand every index
                 * through the file's OWN palette bytes and compare against
                 * the colours the loaded sprite yields. Done with this
                 * check's own arithmetic, not the parser's, for the same
                 * reason this whole block re-parses by hand. Guarded by the
                 * bounds check above so a corrupt data_offset/data_bytes
                 * cannot turn this check into an out-of-bounds read of its
                 * own. */
                if (static_cast<uint64_t>(data_offset) + data_bytes <=
                    raw.size()) {
                    bool payload_matches = true;
                    const uint8_t *pal_raw = raw.data() + data_offset;
                    const uint8_t *idx_raw = pal_raw + palette_padded;
                    for (uint32_t i = 0;
                         i < static_cast<uint32_t>(width) * height; ++i) {
                        const uint8_t slot = idx_raw[i];
                        const uint16_t from_file =
                            static_cast<uint16_t>(pal_raw[slot * 2u]) |
                            static_cast<uint16_t>(pal_raw[slot * 2u + 1u]
                                                   << 8);
                        if (from_file != sprite->palette[sprite->indices[i]]) {
                            payload_matches = false;
                            break;
                        }
                    }
                    check(payload_matches,
                          "every pixel expanded from the file's own palette "
                          "and index bytes matches what the loaded sprite "
                          "yields");
                }
            }
        }
    }

    kf_assets_shutdown();

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves the indexed sprite type decodes: the palette, the frame count, the
 * index data, and -- the point of the whole format -- that expanding index
 * bytes through the palette reproduces exactly the RGB565 the RGB565
 * test_sprite already carries. If the palette lookup were subtly wrong (an
 * off-by-one index, a byte-swapped palette entry, a misplaced frame
 * boundary) this is the check that would catch it, not merely notice that
 * *something* decoded. Mounts examples/hello_sprite/assets_rgb565.kfpack for
 * the RGB565 reference and examples/hello_sprite/assets_indexed.kfpack for
 * the indexed side, both through the runtime override (host_assets.h), and
 * restores the default before returning, so nothing later in this process
 * inherits it and headless_determinism/headless_fullscreen/
 * asset_pipeline_check keep seeing the pack they were checksummed against.
 * Since Task 3 (animated-indexed-sprites plan) converted the default pack
 * itself to indexed, this check can no longer get its RGB565 reference from
 * "the default pack" the way it originally did -- see
 * KF_RGB565_FIXTURE_PACK_PATH's own comment above. */
int run_indexed_asset_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    kf_arena_init_all();

    /* The RGB565 original, read first -- from its own fixture pack, not
     * "the default pack", since Task 3 converted the default pack to
     * indexed (see KF_RGB565_FIXTURE_PACK_PATH's comment above). */
    kf_host_assets_set_pack_path(KF_RGB565_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the RGB565 fixture pack mounts");
    const kf_sprite *rgb = kf_assets_get("test_sprite");
    check(rgb != nullptr && rgb->format == KF_SPRITE_FORMAT_RGB565,
          "the RGB565 fixture's test_sprite is really RGB565");
    std::vector<kf_color> expected;
    if (rgb != nullptr) {
        expected.assign(
            rgb->pixels,
            rgb->pixels + static_cast<size_t>(rgb->width) * rgb->height);
    }
    kf_assets_shutdown();

    /* The indexed fixture. kf_arena_init_all() is NOT called again here --
     * it panics on a second call (kf/arena.cpp), and KF_ARENA_ASSETS is not
     * one of the resettable arenas (only KF_ARENA_SCRATCH is). Mounting a
     * second pack in the same process just grows the same arena's
     * directory-table allocation a little further along; kf_assets_shutdown()
     * above already dropped the first mount's g_up flag so kf_assets_init()
     * below is legal, and the arena is 2MB against a few dozen bytes/row --
     * see kf/assets.h's KF_ASSETS_MAX_ENTRIES comment. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the indexed fixture pack mounts");

    const kf_sprite *ix = kf_assets_get("test_sprite");
    check(ix != nullptr, "kf_assets_get finds the indexed test_sprite");
    if (ix != nullptr) {
        check(ix->format == KF_SPRITE_FORMAT_INDEXED8,
              "it reports KF_SPRITE_FORMAT_INDEXED8");
        check(ix->width == 32u && ix->height == 32u, "it is 32x32");
        check(ix->frame_count == 1u, "it has one frame");
        check(ix->pixels == nullptr, "an indexed sprite has no RGB565 pixels");
        check(ix->indices != nullptr && ix->palette != nullptr,
              "it has both index data and a palette");
        check(ix->palette_count == 32u,
              "the measured palette for this blob is 32 colours");
        check(ix->has_color_key && ix->color_key == ix->palette[0],
              "the colour key is palette entry 0");

        bool identical = (expected.size() ==
                          static_cast<size_t>(ix->width) * ix->height);
        for (size_t i = 0; identical && i < expected.size(); ++i) {
            if (ix->palette[ix->indices[i]] != expected[i]) {
                identical = false;
            }
        }
        check(identical,
              "expanding every index through the palette reproduces the "
              "RGB565 sprite byte for byte -- 8bpp is lossless for this "
              "art");
    }

    const kf_sprite *anim = kf_assets_get("test_sprite_anim");
    check(anim != nullptr, "kf_assets_get finds the 3-frame animation");
    if (anim != nullptr) {
        check(anim->frame_count == 3u, "it reports three frames");
        const size_t stride =
            static_cast<size_t>(anim->width) * anim->height;
        check(std::memcmp(anim->indices, anim->indices + stride, stride) !=
                  0,
              "frame 1 differs from frame 0 -- the fixture is really "
              "animated, not three copies of the same picture");
    }

    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (ok) {
        KF_LOGI(TAG, "indexed-assets: format decodes and is lossless");
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Proves kf_blit_mirrored() (Task 3b): a mirrored blit reverses the sprite's
 * columns rather than its rows, colour-key skip still works mirrored, and
 * clipping is mirror-aware -- the subtle part. An unmirrored sprite hanging
 * off the LEFT screen edge loses its leading (low-index) source columns;
 * mirrored, the same left-edge clip must lose its TRAILING (high-index)
 * source columns instead, because those are what a reversed row puts
 * off-screen, and vice versa on the right edge. See kf/blit.h's own comment
 * on kf_blit_mirrored() for why. Every pixel in the fixture sprite is a
 * distinct value so a wrong column landing somewhere else is caught, not
 * just "something got drawn". */
int run_blit_mirror_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) {
            KF_LOGE(TAG, "FAILED: %s", what);
            ok = false;
        }
    };

    kf_arena_init_all();
    kf_fb_init();

    /* 4 wide x 3 tall, every pixel a distinct value: 0xR0C, row in the high
     * nibble-ish digit, column in the low one, so a test failure's printed
     * value says exactly which source pixel landed in the wrong place. */
    static const kf_color kFixture[3][4] = {
        {0x1000, 0x1001, 0x1002, 0x1003},
        {0x2000, 0x2001, 0x2002, 0x2003},
        {0x3000, 0x3001, 0x3002, 0x3003},
    };
    kf_sprite sprite{};
    sprite.pixels = &kFixture[0][0];
    sprite.width = 4;
    sprite.height = 3;
    sprite.has_color_key = false;
    /* Every kf_sprite construction site sets this explicitly rather than
     * trusting value-init's 0 -- kf/types.h documents frame_count as
     * "always >= 1", and a 0 here is a silent, delayed fault (nothing
     * reads it yet, but Task 2/6 will). */
    sprite.frame_count = 1;

    kf_color *fb = kf_fb_pixels();
    const kf_color kSentinel = 0xDEAD;
    /* All the test's coordinates are non-negative, so the size_t cast is
     * always in range -- this just keeps the index arithmetic in one place
     * instead of repeating the same static_cast at every framebuffer read
     * below. */
    auto px = [&](int screen_x, int screen_y) -> kf_color {
        return fb[static_cast<size_t>(screen_y) * KF_DISPLAY_WIDTH +
                   static_cast<size_t>(screen_x)];
    };

    /* Case 1: fully on-screen, no colour key. Column 0 of the sprite must
     * land at the RIGHTMOST screen column, column (width-1) at the
     * leftmost -- the opposite of kf_blit(). */
    {
        kf_fill(kSentinel);
        const int16_t x = 10, y = 10;
        kf_blit_mirrored(&sprite, x, y);
        bool all_match = true;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                const kf_color got = px(x + col, y + row);
                const kf_color want = kFixture[row][3 - col];
                if (got != want) {
                    KF_LOGE(TAG,
                            "mirror: on-screen (%d,%d) got %04x, want %04x "
                            "(source col %d)",
                            x + col, y + row, got, want, 3 - col);
                    all_match = false;
                }
            }
        }
        check(all_match, "fully on-screen mirrored blit reverses columns");
    }

    /* Case 2: colour key skips the matching pixel, mirrored -- the sentinel
     * underneath must survive at exactly the screen position the keyed
     * source pixel would have mirrored to. */
    {
        kf_sprite keyed = sprite;
        keyed.has_color_key = true;
        keyed.color_key = kFixture[1][1]; /* 0x2001 */

        kf_fill(kSentinel);
        const int16_t x = 20, y = 20;
        kf_blit_mirrored(&keyed, x, y);

        /* Source col 1 mirrors to screen col (width-1-1) = 2. */
        const kf_color at_key = px(x + 2, y + 1);
        check(at_key == kSentinel,
              "colour-keyed pixel is skipped mirrored, leaving the "
              "framebuffer underneath untouched");

        /* A non-keyed neighbour (source col 0 -> screen col 3) still draws. */
        const kf_color neighbour = px(x + 3, y + 1);
        check(neighbour == kFixture[1][0],
              "a non-keyed pixel next to a skipped one still draws mirrored");
    }

    /* Case 3: clipped off the LEFT screen edge. Naively this looks just like
     * kf_blit()'s left-clip, but it must keep the sprite's LOW-index source
     * columns (0..) and drop the HIGH-index ones (they are what a mirrored
     * row pushes further left, off-screen). */
    {
        kf_fill(kSentinel);
        const int16_t x = -2, y = 30; /* 2 of the 4 columns clipped */
        kf_blit_mirrored(&sprite, x, y);
        /* sc(s) = x + width - 1 - s = 1 - s for x=-2, width=4. Visible
         * screen columns 0..1 read source columns 1..0 -- the sprite's
         * LOW-index (leading) columns survive, its HIGH-index (trailing)
         * columns 2..3 are what the mirror pushed off the left edge. */
        const kf_color s0 = px(0, y);
        const kf_color s1 = px(1, y);
        check(s0 == kFixture[0][1],
              "left-edge clip, mirrored: screen col 0 shows source col 1");
        check(s1 == kFixture[0][0],
              "left-edge clip, mirrored: screen col 1 shows source col 0 "
              "(the source's leading column, which survives the clip)");
        /* Source columns 2 and 3 (the sprite's trailing columns) must not
         * have been drawn anywhere -- there is no screen position left of
         * x=0 to check them against directly, so instead confirm nothing
         * beyond the two visible columns changed. */
        const kf_color s2 = px(2, y);
        check(s2 == kSentinel,
              "left-edge clip, mirrored: nothing drawn past the clipped "
              "width");
    }

    /* Case 4: clipped off the RIGHT screen edge -- the mirror of case 3.
     * Must keep the sprite's HIGH-index source columns and drop the
     * LOW-index ones. */
    {
        kf_fill(kSentinel);
        const int16_t x = static_cast<int16_t>(KF_DISPLAY_WIDTH - 2);
        const int16_t y = 40; /* 2 of the 4 columns clipped */
        kf_blit_mirrored(&sprite, x, y);
        /* want.x1 = x+4 = kW+2, clipped to kW, so only screen cols x, x+1
         * survive. Source col (width-1)=3 mirrors to screen col x, source
         * col 2 mirrors to screen col x+1. */
        const kf_color s0 = px(x, y);
        const kf_color s1 = px(x + 1, y);
        check(s0 == kFixture[0][3],
              "right-edge clip, mirrored: leftmost surviving screen col "
              "shows source col 3, the source's trailing column");
        check(s1 == kFixture[0][2],
              "right-edge clip, mirrored: next screen col shows source col "
              "2, not the source's leading column");
        const kf_color before = px(x - 1, y);
        check(before == kSentinel,
              "right-edge clip, mirrored: nothing drawn before the clipped "
              "sprite's left edge");
    }

    /* Case 5: cost accounting. A mirrored blit can never use the memcpy
     * fast path (a reversed row is not a straight copy), so it must be
     * charged to the keyed bucket even when the sprite has no colour key --
     * see kf_draw_count_pixels()'s comment on bucketing by cost shape. */
    {
        kf_draw_counters_reset();
        kf_blit_mirrored(&sprite, 50, 50); /* fully on-screen, unkeyed */
        const kf_draw_counters counters = kf_draw_counters_get();
        check(counters.keyed_pixels == 12u && counters.opaque_pixels == 0u,
              "an unkeyed mirrored blit is still charged to keyed_pixels, "
              "the per-pixel-cost bucket, not opaque_pixels");
    }

    if (ok) {
        KF_LOGI(TAG, "blit-mirror: reversal, colour-key, both clip edges "
                     "and cost accounting all hold");
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* The centrepiece losslessness proof, and it does not need a golden
 * constant to make it: draw the RGB565 test_sprite and the indexed one at
 * the same place into the same framebuffer, and compare the two results
 * byte for byte. If 8bpp indexing lost anything for this art, this fails.
 * The RGB565 side is read from its own dedicated fixture pack, not "the
 * default pack" -- since Task 3 (animated-indexed-sprites plan) converted
 * the default pack to indexed, the default pack can no longer supply an
 * RGB565 sprite at all (see KF_RGB565_FIXTURE_PACK_PATH's own comment).
 *
 * Also pins frame addressing (frame k reads k*w*h into the payload), the
 * out-of-range clamp, mirrored equivalence, and the draw-counter bucket. */
int run_indexed_blit_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_arena_init_all();
    kf_fb_init();

    kf_host_assets_set_pack_path(KF_RGB565_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the RGB565 fixture pack mounts");
    const kf_sprite *rgb = kf_assets_get("test_sprite");
    check(rgb != nullptr && rgb->format == KF_SPRITE_FORMAT_RGB565,
          "RGB565 test_sprite found, and really RGB565");

    const size_t fb_bytes =
        static_cast<size_t>(KF_DISPLAY_WIDTH) * KF_DISPLAY_HEIGHT * sizeof(kf_color);
    std::vector<uint8_t> from_rgb(fb_bytes);
    std::vector<uint8_t> from_indexed(fb_bytes);

    if (rgb != nullptr) {
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit(rgb, 40, 50);
        std::memcpy(from_rgb.data(), kf_fb_pixels(), fb_bytes);
    }
    kf_assets_shutdown();

    /* kf_arena_init_all() is NOT called again here, or anywhere else in this
     * function -- it panics on a second call (kf/arena.cpp:
     * `KF_ASSERT(!g_initialised, "kf_arena_init_all called twice")`), and
     * KF_ARENA_ASSETS is not one of the resettable arenas (only
     * KF_ARENA_SCRATCH is). One kf_arena_init_all() call at the top of this
     * function is enough for every kf_assets_shutdown()/kf_assets_init()
     * remount below -- see run_indexed_asset_check() (Task 1) for where
     * this was found the hard way. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture mounts");
    const kf_sprite *ix = kf_assets_get("test_sprite");
    check(ix != nullptr, "indexed test_sprite found");

    if (ix != nullptr) {
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit(ix, 40, 50);
        std::memcpy(from_indexed.data(), kf_fb_pixels(), fb_bytes);
        check(from_rgb == from_indexed,
              "an indexed blit and an RGB565 blit of the same sprite produce "
              "a byte-identical framebuffer");

        /* Mirrored, same claim. */
        kf_fill(KF_RGB(8, 16, 24));
        kf_blit_mirrored(ix, 40, 50);
        std::vector<uint8_t> ix_mirror(fb_bytes);
        std::memcpy(ix_mirror.data(), kf_fb_pixels(), fb_bytes);
        kf_assets_shutdown();
        kf_host_assets_set_pack_path(KF_RGB565_FIXTURE_PACK_PATH);
        check(kf_assets_init() == KF_OK, "the RGB565 fixture pack remounts");
        const kf_sprite *rgb2 = kf_assets_get("test_sprite");
        kf_fill(KF_RGB(8, 16, 24));
        if (rgb2 != nullptr) { kf_blit_mirrored(rgb2, 40, 50); }
        check(std::memcmp(ix_mirror.data(), kf_fb_pixels(), fb_bytes) == 0,
              "a mirrored indexed blit matches a mirrored RGB565 blit");
        kf_assets_shutdown();
    }

    /* Frames. */
    kf_host_assets_set_pack_path(KF_INDEXED_FIXTURE_PACK_PATH);
    check(kf_assets_init() == KF_OK, "indexed fixture remounts");
    const kf_sprite *anim = kf_assets_get("test_sprite_anim");
    check(anim != nullptr && anim->frame_count == 3u, "3-frame fixture found");
    if (anim != nullptr) {
        std::vector<uint8_t> f0(fb_bytes), f1(fb_bytes), f_oob(fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 0);
        std::memcpy(f0.data(), kf_fb_pixels(), fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 1);
        std::memcpy(f1.data(), kf_fb_pixels(), fb_bytes);
        kf_fill(KF_BLACK); kf_blit_frame(anim, 40, 50, 99);
        std::memcpy(f_oob.data(), kf_fb_pixels(), fb_bytes);

        check(f0 != f1, "frame 1 draws something different from frame 0");
        check(f0 == f_oob,
              "an out-of-range frame clamps to frame 0 rather than wrapping "
              "-- a wrap would hide a stale cursor behind plausible-looking "
              "animation");

        kf_fill(KF_BLACK);
        kf_draw_counters_reset();
        kf_blit_frame(anim, 40, 50, 0);
        const kf_draw_counters counters = kf_draw_counters_get();
        check(counters.opaque_pixels == 0u &&
                  counters.keyed_pixels ==
                      static_cast<uint32_t>(anim->width) * anim->height,
              "an indexed blit is charged entirely to the keyed bucket -- it "
              "has no memcpy fast path, so its cost SHAPE is per-pixel "
              "whether or not a key is tested");
    }

    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (!ok) { return 1; }
    KF_LOGI(TAG, "indexed-blit: pixel-identical to RGB565, frames address "
                  "correctly");
    return 0;
}

/* Task 2 of the Lua game layer plan (docs/superpowers/plans/
 * 2026-08-12-lua-game-layer.md): proves the retained scene differ
 * (kf/scene.h) in isolation. No Lua exists yet and nothing else in
 * hakoniwaos/ calls this module -- Task 3 adds the Lua binding, Task 4
 * rebuilds the C++ creature screen on top of it. Four checks, in the order
 * the plan states them, and the first is the most important:
 *
 *   1. A committed frame with no changes marks zero dirty rectangles and
 *      draws zero pixels -- the differ's entire reason to exist.
 *   2. Moving one object into an overlapping position marks ONE dirty
 *      rectangle spanning both the old and the new position, because
 *      framebuffer.cpp's own touches_or_overlaps() merges them.
 *   3. THE REAL PROOF, and the one that needs no golden constant: a scene
 *      committed through kf_scene_commit() is memcmp-identical to the same
 *      picture drawn by hand, in the same order, with kf_fill_rect() and
 *      kf_blit_frame() called directly. Two objects at different layers,
 *      so paint ORDER is actually exercised, not just final coverage.
 *   4. Twelve independently-moving objects coalesce to at most
 *      KF_MAX_DIRTY_RECTS rectangles covering well under the whole
 *      framebuffer -- proving kf_scene_commit()'s own coalescer beats
 *      kf_fb_mark_dirty()'s full-screen fallback on a scene shaped
 *      specifically to trigger it (24 raw candidates, spread far enough
 *      apart that none merge for free the way check 2's did).
 *   5. A text object that never sets its own colours paints white on black
 *      -- kf_scene_add_text() takes no colour arguments, so scene.cpp's
 *      field initialiser is the whole of that contract, and nothing else
 *      in the suite would notice it changing.
 *
 * A further thing the task brief asks for -- that this check FAILS if
 * kf_scene_commit()'s body is deleted -- is not code here: it was verified
 * by hand, once, by actually deleting that body, rebuilding, watching this
 * check go red, and reverting. A check cannot prove its own vacuity by
 * asserting about itself; see this plan's own banner on tests that quietly
 * stopped testing what they were written for. */
static int run_scene_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_arena_init_all();
    kf_fb_init();
    kf_fb_clear_dirty(); /* kf_fb_init() marks the whole screen dirty as its
                           * own starting state (framebuffer.cpp); clear it
                           * so what follows only ever sees marks THIS check
                           * made. */

    kf_host_assets_set_pack_path(KF_CREATURE_DEMO_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the creature demo pack mounts");
    const kf_sprite *egg = kf_assets_get("egg_idle_s");
    check(egg != nullptr, "egg_idle_s resolves in the mounted pack");

    /* ---- 1. A commit with nothing changed draws nothing. ---- */
    kf_scene_reset();
    const kf_scene_id box1 = kf_scene_add_box(20, 20, KF_RGB(0, 200, 0));
    kf_scene_set_pos(box1, 10, 10);
    kf_scene_commit(); /* first commit ever for this object: forced full
                         * redraw (kf_scene_reset()'s own contract), so this
                         * one legitimately draws -- what matters is the
                         * NEXT commit, below. */
    kf_fb_clear_dirty(); /* simulate "the frame loop just presented" */

    kf_draw_counters_reset();
    kf_scene_commit(); /* nothing declared has changed since the commit above */
    const kf_draw_counters idle_counters = kf_draw_counters_get();
    check(idle_counters.opaque_pixels == 0u && idle_counters.keyed_pixels == 0u,
          "a commit with no changes draws zero pixels");
    check(kf_fb_dirty_rects().count == 0,
          "a commit with no changes marks zero dirty rectangles");

    /* ---- 2. Moving one object into an overlapping position merges old and
     * new into one rectangle. Dirty state is already clean (0 rects) from
     * check 1 immediately above. ---- */
    const kf_rect old_bounds = kf_scene_bounds(box1); /* {10,10,30,30} */
    kf_scene_set_pos(box1, 16, 16); /* new: {16,16,36,36} -- overlaps old_bounds */
    const kf_rect new_bounds = kf_scene_bounds(box1);
    kf_scene_commit();
    const kf_dirty_rects moved = kf_fb_dirty_rects();
    check(moved.count == 1,
          "moving one object into an overlapping position marks exactly "
          "one merged dirty rectangle, not two separate ones");
    if (moved.count == 1) {
        const kf_rect expect = kf_rect_union(old_bounds, new_bounds);
        check(moved.rects[0].x0 == expect.x0 && moved.rects[0].y0 == expect.y0 &&
                  moved.rects[0].x1 == expect.x1 && moved.rects[0].y1 == expect.y1,
              "the merged rectangle's bounds cover both the old and the new "
              "position");
    }

    /* ---- 3. The real proof: memcmp against a hand-drawn reference. ---- */
    kf_scene_reset();
    kf_scene_set_background_color(KF_RGB(20, 24, 28));
    const kf_scene_id back_box = kf_scene_add_box(60, 60, KF_RGB(0, 80, 160));
    kf_scene_set_pos(back_box, 40, 40);
    kf_scene_set_layer(back_box, 0);
    const kf_scene_id front_sprite = kf_scene_add_sprite("egg_idle_s");
    kf_scene_set_pos(front_sprite, 60, 60); /* overlaps back_box */
    kf_scene_set_layer(front_sprite, 1);
    kf_scene_commit();

    const size_t fb_bytes = KF_FRAMEBUFFER_BYTES;
    std::vector<uint8_t> from_scene(fb_bytes);
    std::memcpy(from_scene.data(), kf_fb_pixels(), fb_bytes);

    /* Hand-drawn, same order the scene painted them in above: background,
     * then the box (layer 0), then the sprite (layer 1). Both fully
     * repaint the screen -- a full-screen fill plus two bounded blits --
     * so whatever was in the framebuffer beforehand does not matter. */
    kf_fill_rect(kf_rect{0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
                          static_cast<int16_t>(KF_DISPLAY_HEIGHT)},
                 KF_RGB(20, 24, 28));
    kf_fill_rect(kf_rect{40, 40, 100, 100}, KF_RGB(0, 80, 160));
    if (egg != nullptr) {
        kf_blit_frame(egg, 60, 60, 0);
    }
    std::vector<uint8_t> by_hand(fb_bytes);
    std::memcpy(by_hand.data(), kf_fb_pixels(), fb_bytes);

    check(std::memcmp(from_scene.data(), by_hand.data(), fb_bytes) == 0,
          "a scene committed through kf_scene_commit() is memcmp-identical "
          "to the same picture drawn by hand, in the same order, with "
          "kf_fill_rect()/kf_blit_frame() directly");

    /* ---- 4. Twelve independently-moving objects coalesce; they do not
     * collapse the framebuffer to its own full-screen fallback. ---- */
    kf_scene_reset();
    kf_scene_id spread[12];
    constexpr int16_t kBoxSize = 10;
    for (int i = 0; i < 12; ++i) {
        /* A 4x3 grid across the 240x320 panel, 60px apart horizontally and
         * 100px vertically against a 10px box -- far enough that no two
         * objects' old OR new positions touch or overlap each other. This
         * is deliberately the opposite shape from check 2's overlapping
         * move: nothing here can merge for free. */
        const int col = i % 4;
        const int row = i / 4;
        const int16_t x = static_cast<int16_t>(10 + col * 60);
        const int16_t y = static_cast<int16_t>(10 + row * 100);
        spread[i] = kf_scene_add_box(kBoxSize, kBoxSize, KF_RGB(200, 200, 0));
        kf_scene_set_pos(spread[i], x, y);
    }
    kf_scene_commit(); /* establishes presented state for all 12 */
    kf_fb_clear_dirty(); /* only the SECOND commit's marks matter below */

    for (int i = 0; i < 12; ++i) {
        const int col = i % 4;
        const int row = i / 4;
        /* Shift 30px right -- still inside this object's own grid cell, so
         * its own old and new rectangles do not overlap, and the new
         * position does not reach the next object's cell either. */
        const int16_t x = static_cast<int16_t>(10 + col * 60 + 30);
        const int16_t y = static_cast<int16_t>(10 + row * 100);
        kf_scene_set_pos(spread[i], x, y);
    }
    kf_scene_commit();

    const kf_dirty_rects spread_dirty = kf_fb_dirty_rects();
    check(spread_dirty.count >= 1 && spread_dirty.count <= KF_MAX_DIRTY_RECTS,
          "12 independently-moving objects coalesce to at most "
          "KF_MAX_DIRTY_RECTS dirty rectangles");
    uint32_t total_area = 0;
    for (int i = 0; i < spread_dirty.count; ++i) {
        total_area += kf_rect_area(spread_dirty.rects[i]);
    }
    const size_t total_bytes =
        static_cast<size_t>(total_area) * sizeof(kf_color);
    check(total_bytes < KF_FRAMEBUFFER_BYTES / 4u,
          "the coalesced dirty area stays well under the full framebuffer "
          "-- proving the coalescer beat kf_fb_mark_dirty()'s own "
          "full-screen fallback rather than merely avoiding it by luck");
    KF_LOGI(TAG,
            "scene: 12-object move -> %d dirty rect(s), %zu of %d "
            "framebuffer bytes",
            spread_dirty.count, total_bytes, KF_FRAMEBUFFER_BYTES);

    /* ---- 5. A text object that never calls kf_scene_set_colors() paints
     * white on black. kf_scene_add_text() takes no colour arguments, so this
     * default is the ONLY thing standing between `kf.text("HI")` in a script
     * that never calls `:color()` and invisible black-on-black text -- and
     * it lives in one field initialiser inside scene.cpp, where nothing else
     * would notice it changing. ADR 0040's "Two RenderStates per object"
     * section is the reason that initialiser is under pressure: it is the
     * single non-zero one in the whole SceneObject, so it is what decides
     * whether g_objects lands in .bss or .data. Pinning the OBSERVABLE
     * default here means that section can be fixed without the fix being
     * allowed to quietly change what a script sees. ---- */
    kf_scene_reset();
    kf_scene_set_background_color(KF_BLACK);
    const kf_scene_id label = kf_scene_add_text("HI");
    kf_scene_set_pos(label, 12, 24);
    kf_scene_commit();

    std::vector<uint8_t> default_text(fb_bytes);
    std::memcpy(default_text.data(), kf_fb_pixels(), fb_bytes);

    kf_fill_rect(kf_rect{0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
                          static_cast<int16_t>(KF_DISPLAY_HEIGHT)},
                 KF_BLACK);
    std::vector<uint8_t> bare_bg(fb_bytes);
    std::memcpy(bare_bg.data(), kf_fb_pixels(), fb_bytes);
    kf_text_draw(12, 24, "HI", KF_WHITE, KF_BLACK);
    std::vector<uint8_t> white_on_black(fb_bytes);
    std::memcpy(white_on_black.data(), kf_fb_pixels(), fb_bytes);

    /* Non-vacuity first: if the font drew nothing at all, the comparison
     * below would pass against a black-on-black scene for the wrong reason. */
    check(std::memcmp(white_on_black.data(), bare_bg.data(), fb_bytes) != 0,
          "the reference text actually marks pixels -- so the default-colour "
          "comparison below is not comparing two blank screens");
    check(std::memcmp(default_text.data(), white_on_black.data(), fb_bytes) == 0,
          "a text object that never calls kf_scene_set_colors() paints white "
          "on black, the same as kf_text_draw(..., KF_WHITE, KF_BLACK)");

    kf_scene_reset();
    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (ok) {
        KF_LOGI(TAG, "scene: differ proved against a hand-drawn reference, "
                      "coalescer beats the framebuffer's own fallback");
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Task 3 of the Lua game layer plan: the Lua binding over kf/scene.h
 * (sdk/lua/kf_lua_scene.cpp), proved the same way run_scene_check() proves
 * Core itself -- memcmp against a hand-drawn reference -- but with a script
 * declaring the scene instead of this file calling kf_scene_*() directly.
 * See docs/architecture/adr-0041-lua-drawing-binding.md. */
static int run_lua_draw_check(void) {
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        if (!cond) { KF_LOGE(TAG, "FAILED: %s", what); ok = false; }
    };

    kf_arena_init_all();
    kf_fb_init();
    kf_fb_clear_dirty();
    kf_scene_reset();

    kf_host_assets_set_pack_path(KF_CREATURE_DEMO_PACK_PATH);
    check(kf_assets_init() == KF_OK, "the creature demo pack mounts");
    const kf_sprite *egg = kf_assets_get("egg_idle_s");
    check(egg != nullptr, "egg_idle_s resolves in the mounted pack");

    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);

    /* ---- 1. A background and one sprite, declared from Lua, committed,
     * and memcmp-identical to the same picture drawn by hand with
     * kf_fill_rect()/kf_blit_frame() -- exactly run_scene_check()'s check
     * 3, but with a script in the loop instead of this file calling
     * kf_scene_*() directly. ---- */
    constexpr const char *kDrawScript = R"lua(
kf.background(kf.color(20, 24, 28))
body = kf.sprite("egg_idle_s")
body:move(60, 60)
)lua";
    check(kf_lua_port_init(kDrawScript, "=lua_draw_check"),
          "the draw-check script loads and its top-level code runs");
    kf_lua_port_frame(kFixedDtMs);
    kf_scene_commit();

    const size_t fb_bytes = KF_FRAMEBUFFER_BYTES;
    std::vector<uint8_t> from_lua(fb_bytes);
    std::memcpy(from_lua.data(), kf_fb_pixels(), fb_bytes);

    kf_fill_rect(kf_rect{0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
                          static_cast<int16_t>(KF_DISPLAY_HEIGHT)},
                 KF_RGB(20, 24, 28));
    if (egg != nullptr) {
        kf_blit_frame(egg, 60, 60, 0);
    }
    std::vector<uint8_t> by_hand(fb_bytes);
    std::memcpy(by_hand.data(), kf_fb_pixels(), fb_bytes);

    check(std::memcmp(from_lua.data(), by_hand.data(), fb_bytes) == 0,
          "a scene declared from Lua and committed is memcmp-identical to "
          "the same picture drawn by hand with kf_fill_rect()/"
          "kf_blit_frame() directly");

    /* ---- 2. A second commit with no script changes draws zero pixels --
     * the same headline property run_scene_check()'s check 1 proves for
     * Core alone. ---- */
    kf_fb_clear_dirty();
    kf_draw_counters_reset();
    kf_scene_commit();
    const kf_draw_counters idle = kf_draw_counters_get();
    check(idle.opaque_pixels == 0u && idle.keyed_pixels == 0u,
          "a second commit with no script changes draws zero pixels");
    check(kf_fb_dirty_rects().count == 0,
          "a second commit with no script changes marks zero dirty "
          "rectangles");

    kf_lua_port_shutdown();

    /* ---- 3. A bad sprite name draws the magenta placeholder, not
     * nothing -- CLAUDE.md's own "a mistyped sprite name should say so"
     * rule, checked on the panel rather than merely asserted. Core logs
     * the name (hakoniwaos/src/scene.cpp's resolve_sprite()); this checks
     * the pixel. ---- */
    kf_scene_reset();
    constexpr const char *kBadSpriteScript = R"lua(
kf.background(kf.color(20, 24, 28))
body = kf.sprite("does_not_exist_in_the_pack")
body:move(60, 60)
)lua";
    check(kf_lua_port_init(kBadSpriteScript, "=lua_draw_check_bad_sprite"),
          "the bad-sprite-name script loads");
    kf_lua_port_frame(kFixedDtMs);
    kf_scene_commit();
    const kf_color *pixels =
        reinterpret_cast<const kf_color *>(kf_fb_pixels());
    /* (70, 70) is inside the 48x48 sprite bounds declared at (60, 60). */
    const kf_color sampled = pixels[70 * KF_DISPLAY_WIDTH + 70];
    check(sampled == KF_RGB(255, 0, 128),
          "a bad sprite name draws the magenta placeholder rather than "
          "nothing");
    kf_lua_port_shutdown();

    /* ---- 4. 64 live objects (KF_SCENE_MAX_OBJECTS) keep Lua's own arena
     * well under the quarter-of-the-arena ceiling the plan sets, leaving
     * the script itself three quarters. ---- */
    kf_scene_reset();
    constexpr const char *kManyObjectsScript = R"lua(
kf.background(kf.color(0, 0, 0))
objs = {}
for i = 1, 64 do
    objs[i] = kf.sprite("egg_idle_s")
    objs[i]:move(i, i)
end
)lua";
    check(kf_lua_port_init(kManyObjectsScript, "=lua_draw_check_64_objects"),
          "a script creating 64 objects (KF_SCENE_MAX_OBJECTS) loads "
          "without error");
    const kf_lua_alloc_stats stats64 = kf_lua_alloc_get_stats();
    constexpr size_t kLiveBytesCeiling = 256u * 1024u;
    check(stats64.live_bytes < kLiveBytesCeiling,
          "kf_lua_alloc_get_stats().live_bytes stays under 256KB with 64 "
          "scene objects live");
    KF_LOGI(TAG, "lua-draw: 64 live scene objects -> %zu live bytes (< %zu)",
            stats64.live_bytes, kLiveBytesCeiling);
    kf_lua_port_shutdown();

    /* ---- 5. Anti-vacuity: the 65th object raises a script error naming
     * the limit, rather than the top-level chunk quietly finishing with a
     * userdata missing its methods. Prove kf_lua_port_init() itself
     * reports the failure, not just that some later call would have. ---- */
    kf_scene_reset();
    constexpr const char *kOverflowScript = R"lua(
objs = {}
for i = 1, 65 do
    objs[i] = kf.sprite("egg_idle_s")
end
)lua";
    const bool overflow_loaded =
        kf_lua_port_init(kOverflowScript, "=lua_draw_check_overflow");
    check(!overflow_loaded,
          "the 65th kf.sprite() call raises a script error naming "
          "KF_SCENE_MAX_OBJECTS rather than succeeding");
    if (overflow_loaded) {
        /* Should not happen -- see the check above -- but if the limit
         * were ever silently lifted, tearing the VM down cleanly here
         * keeps the rest of this function's state sane rather than
         * compounding one failure into a second, unrelated one below. */
        kf_lua_port_shutdown();
    }

    /* ---- 6. Anti-vacuity: a method called on a removed object raises a
     * script error rather than silently operating on a slot something
     * else now owns. The script catches its own error with pcall() and
     * reports which happened through kf.report() -- proof this binding's
     * error, not some unrelated failure, is what fired. ---- */
    kf_scene_reset();
    constexpr const char *kRemovedScript = R"lua(
o = kf.sprite("egg_idle_s")
o:remove()
local ok = pcall(function() o:move(1, 2) end)
kf.report(ok and 1 or 0)
)lua";
    check(kf_lua_port_init(kRemovedScript, "=lua_draw_check_removed"),
          "the removed-object script loads");
    check(kf_lua_port_last_report() == 0,
          "calling a method on a removed object raises a script error, "
          "caught by the script's own pcall rather than crashing the VM");
    kf_lua_port_shutdown();

    /* ---- 7. The minimal-pet example (examples/hello_pet/pet.lua), read
     * from disk and run verbatim -- the plan's own acceptance test for
     * this API (docs/superpowers/plans/2026-08-12-lua-game-layer.md, "The
     * minimal pet, in Lua": "if it does not work verbatim ... one of the
     * two is wrong"). Needs a live pet session -- pet.stage()/pet.hunger()
     * are read every frame, pet.feed()/pet.play() are wired to the two
     * button handlers -- unlike every check above, so this brings its own
     * store/power/session lifecycle up and back down around it. ---- */
    {
        std::ifstream pet_lua_file(KF_HELLO_PET_SCRIPT_PATH);
        check(pet_lua_file.good(), "examples/hello_pet/pet.lua opens");
        const std::string pet_lua_source(
            (std::istreambuf_iterator<char>(pet_lua_file)),
            std::istreambuf_iterator<char>());

        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() /
            ("kamiframe-headless-lua-draw-hello-pet-" +
             std::to_string(KF_GETPID()));
        std::error_code rm_ec;
        std::filesystem::remove_all(dir, rm_ec); /* in case a prior run crashed */
        kf_host_storage_set_dir(dir.string().c_str());
        check(kf_store_init() == KF_OK, "kf_store_init (hello_pet)");
        check(kf_power_init() == KF_OK, "kf_power_init (hello_pet)");
        kf_pet_session_init();

        kf_scene_reset();
        check(kf_lua_port_init(pet_lua_source.c_str(),
                                "=examples/hello_pet/pet.lua"),
              "examples/hello_pet/pet.lua loads and its top-level code "
              "runs, verbatim from the plan");
        for (int i = 0; i < 5; ++i) {
            kf_pet_session_frame(kFixedDtMs);
            kf_lua_port_frame(kFixedDtMs);
            kf_scene_commit();
        }
        check(kf_lua_port_frame_count() == 5u,
              "examples/hello_pet/pet.lua's on_frame runs for 5 frames "
              "with no script error");
        kf_lua_port_shutdown();

        kf_pet_session_shutdown();
        kf_power_shutdown();
        kf_store_shutdown();
        std::filesystem::remove_all(dir, rm_ec);
    }

    kf_scene_reset();
    kf_assets_shutdown();
    kf_host_assets_set_pack_path(nullptr);

    if (ok) {
        KF_LOGI(TAG, "lua-draw: a scene declared from Lua matches the "
                      "hand-drawn reference; idle, placeholder, overflow, "
                      "removed-object and hello_pet all behave as "
                      "specified");
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
    bool verify_pet = false;
    bool verify_pet_stage = false;
    bool verify_tree_shape = false;
    bool verify_hokorimaru = false;
    bool verify_pet_personality = false;
    bool verify_mess = false;
    bool verify_dirtiness = false;
    bool verify_pet_sickness = false;
    bool verify_pet_death = false;
    bool verify_pet_preferences = false;
    bool verify_pet_care_variation = false;
    bool verify_pet_adult_reachability = false;
    bool verify_pet_debug_jump = false;
    bool verify_creature_pose = false;
    bool verify_creature_wander = false;
    bool verify_creature_screen = false;
    bool verify_creature_screen_sprites = false;
    bool verify_creature_screen_input = false;
    bool verify_creature_screen_egg = false;
    bool verify_creature_screen_death = false;
    bool verify_creature_screen_debug_jump = false;
    bool verify_creature_screen_stats = false;
    bool verify_lua_pet = false;
    bool verify_pet_screen = false;
    bool verify_demand_curve = false;
    bool verify_screen_nav = false;
    bool verify_lua_creature = false;
    bool verify_assets = false;
    bool verify_indexed_assets = false;
    bool verify_blit_mirror = false;
    bool verify_indexed_blit = false;
    bool verify_creature_anim = false;
    bool verify_frame_counters = false;
    bool verify_creature_screen_budget_combo = false;
    bool verify_scene = false;
    bool verify_lua_draw = false;
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
        } else if (std::strcmp(argv[i], "--verify-pet-stage") == 0) {
            verify_pet_stage = true;
        } else if (std::strcmp(argv[i], "--verify-tree-shape") == 0) {
            verify_tree_shape = true;
        } else if (std::strcmp(argv[i], "--verify-hokorimaru") == 0) {
            verify_hokorimaru = true;
        } else if (std::strcmp(argv[i], "--verify-pet-personality") == 0) {
            verify_pet_personality = true;
        } else if (std::strcmp(argv[i], "--verify-mess") == 0) {
            verify_mess = true;
        } else if (std::strcmp(argv[i], "--verify-dirtiness") == 0) {
            verify_dirtiness = true;
        } else if (std::strcmp(argv[i], "--verify-pet-sickness") == 0) {
            verify_pet_sickness = true;
        } else if (std::strcmp(argv[i], "--verify-pet-death") == 0) {
            verify_pet_death = true;
        } else if (std::strcmp(argv[i], "--verify-pet-preferences") == 0) {
            verify_pet_preferences = true;
        } else if (std::strcmp(argv[i], "--verify-pet-care-variation") == 0) {
            verify_pet_care_variation = true;
        } else if (std::strcmp(argv[i], "--verify-pet-adult-reachability") ==
                   0) {
            verify_pet_adult_reachability = true;
        } else if (std::strcmp(argv[i], "--verify-pet-debug-jump") == 0) {
            verify_pet_debug_jump = true;
        } else if (std::strcmp(argv[i], "--verify-creature-pose") == 0) {
            verify_creature_pose = true;
        } else if (std::strcmp(argv[i], "--verify-creature-wander") == 0) {
            verify_creature_wander = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen") == 0) {
            verify_creature_screen = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen-sprites") ==
                   0) {
            verify_creature_screen_sprites = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen-input") ==
                   0) {
            verify_creature_screen_input = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen-egg") ==
                   0) {
            verify_creature_screen_egg = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen-death") ==
                   0) {
            verify_creature_screen_death = true;
        } else if (std::strcmp(argv[i],
                                "--verify-creature-screen-debug-jump") == 0) {
            verify_creature_screen_debug_jump = true;
        } else if (std::strcmp(argv[i], "--verify-creature-screen-stats") ==
                   0) {
            verify_creature_screen_stats = true;
        } else if (std::strcmp(argv[i], "--verify-lua-pet") == 0) {
            verify_lua_pet = true;
        } else if (std::strcmp(argv[i], "--dump-fb") == 0 && i + 1 < argc) {
            g_dump_path = argv[++i];
        } else if (std::strcmp(argv[i], "--verify-pet-screen") == 0) {
            verify_pet_screen = true;
        } else if (std::strcmp(argv[i], "--verify-demand-curve") == 0) {
            verify_demand_curve = true;
        } else if (std::strcmp(argv[i], "--verify-screen-nav") == 0) {
            verify_screen_nav = true;
        } else if (std::strcmp(argv[i], "--verify-lua-creature") == 0) {
            verify_lua_creature = true;
        } else if (std::strcmp(argv[i], "--verify-assets") == 0) {
            verify_assets = true;
        } else if (std::strcmp(argv[i], "--verify-indexed-assets") == 0) {
            verify_indexed_assets = true;
        } else if (std::strcmp(argv[i], "--verify-blit-mirror") == 0) {
            verify_blit_mirror = true;
        } else if (std::strcmp(argv[i], "--verify-indexed-blit") == 0) {
            verify_indexed_blit = true;
        } else if (std::strcmp(argv[i], "--verify-creature-anim") == 0) {
            verify_creature_anim = true;
        } else if (std::strcmp(argv[i], "--verify-frame-counters") == 0) {
            verify_frame_counters = true;
        } else if (std::strcmp(argv[i],
                                "--verify-creature-screen-budget-combo") ==
                   0) {
            verify_creature_screen_budget_combo = true;
        } else if (std::strcmp(argv[i], "--verify-scene") == 0) {
            verify_scene = true;
        } else if (std::strcmp(argv[i], "--verify-lua-draw") == 0) {
            verify_lua_draw = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("kamiframe-headless [--frames N] [--seed N] "
                        "[--expect-checksum HEX] [--max-dirty-percent N] [--stress]\n"
                        "kamiframe-headless --verify-storage-power\n"
                        "kamiframe-headless --verify-lvgl [--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua [--frames N]\n"
                        "kamiframe-headless --verify-pet\n"
                        "kamiframe-headless --verify-pet-stage\n"
                        "kamiframe-headless --verify-tree-shape\n"
                        "kamiframe-headless --verify-hokorimaru\n"
                        "kamiframe-headless --verify-pet-personality\n"
                        "kamiframe-headless --verify-mess\n"
                        "kamiframe-headless --verify-dirtiness\n"
                        "kamiframe-headless --verify-pet-sickness\n"
                        "kamiframe-headless --verify-pet-death\n"
                        "kamiframe-headless --verify-pet-preferences\n"
                        "kamiframe-headless --verify-pet-care-variation\n"
                        "kamiframe-headless --verify-pet-adult-reachability\n"
                        "kamiframe-headless --verify-pet-debug-jump\n"
                        "kamiframe-headless --verify-creature-pose\n"
                        "kamiframe-headless --verify-creature-wander\n"
                        "kamiframe-headless --verify-creature-screen\n"
                        "kamiframe-headless --verify-creature-screen-sprites\n"
                        "kamiframe-headless --verify-creature-screen-input\n"
                        "kamiframe-headless --verify-creature-screen-egg\n"
                        "kamiframe-headless --verify-creature-screen-death\n"
                        "kamiframe-headless "
                        "--verify-creature-screen-debug-jump\n"
                        "kamiframe-headless --verify-creature-screen-stats\n"
                        "kamiframe-headless --verify-lua-pet\n"
                        "kamiframe-headless --verify-pet-screen "
                        "[--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-demand-curve\n"
                        "kamiframe-headless --verify-screen-nav "
                        "[--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua-creature\n"
                        "kamiframe-headless --verify-assets\n"
                        "kamiframe-headless --verify-indexed-assets\n"
                        "kamiframe-headless --verify-blit-mirror\n"
                        "kamiframe-headless --verify-indexed-blit\n"
                        "kamiframe-headless --verify-creature-anim\n"
                        "kamiframe-headless --verify-frame-counters\n"
                        "kamiframe-headless "
                        "--verify-creature-screen-budget-combo\n"
                        "kamiframe-headless --verify-scene\n"
                        "kamiframe-headless --verify-lua-draw\n");
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

    if (verify_pet_stage) {
        return run_pet_stage_check();
    }

    if (verify_tree_shape) {
        return run_evolution_tree_shape_check();
    }

    if (verify_hokorimaru) {
        return run_hokorimaru_check();
    }

    if (verify_pet_personality) {
        return run_pet_personality_check();
    }

    if (verify_mess) {
        return run_pet_mess_check();
    }

    if (verify_dirtiness) {
        return run_pet_dirtiness_check();
    }

    if (verify_pet_sickness) {
        return run_pet_sickness_check();
    }

    if (verify_pet_death) {
        return run_pet_death_check();
    }

    if (verify_pet_preferences) {
        return run_pet_preferences_check();
    }

    if (verify_pet_care_variation) {
        return run_pet_care_variation_check();
    }

    if (verify_pet_adult_reachability) {
        return run_pet_adult_reachability_check();
    }

    if (verify_pet_debug_jump) {
        return run_pet_debug_jump_check();
    }

    if (verify_creature_pose) {
        return run_creature_pose_check();
    }

    if (verify_creature_wander) {
        return run_creature_wander_check();
    }

    if (verify_creature_screen) {
        return run_creature_screen_check();
    }

    if (verify_creature_screen_sprites) {
        return run_creature_screen_sprite_check();
    }

    if (verify_creature_screen_input) {
        return run_creature_screen_input_check();
    }

    if (verify_creature_screen_egg) {
        return run_creature_screen_egg_check();
    }

    if (verify_creature_screen_death) {
        return run_creature_screen_death_check();
    }

    if (verify_creature_screen_debug_jump) {
        return run_creature_screen_debug_jump_check();
    }

    if (verify_creature_screen_stats) {
        return run_creature_screen_stats_check();
    }

    if (verify_lua_pet) {
        return run_lua_pet_check();
    }

    if (verify_pet_screen) {
        return run_pet_screen_check(expect_checksum, have_expect);
    }

    if (verify_demand_curve) {
        return run_pet_demand_curve_check();
    }

    if (verify_screen_nav) {
        return run_screen_nav_check(expect_checksum, have_expect);
    }

    if (verify_lua_creature) {
        return run_lua_creature_check();
    }

    if (verify_assets) {
        return run_asset_check();
    }

    if (verify_indexed_assets) {
        return run_indexed_asset_check();
    }

    if (verify_blit_mirror) {
        return run_blit_mirror_check();
    }

    if (verify_indexed_blit) {
        return run_indexed_blit_check();
    }

    if (verify_creature_anim) {
        return run_creature_anim_check();
    }

    if (verify_frame_counters) {
        return run_frame_counters_check();
    }

    if (verify_creature_screen_budget_combo) {
        return run_creature_screen_budget_combination_check();
    }

    if (verify_scene) {
        return run_scene_check();
    }

    if (verify_lua_draw) {
        return run_lua_draw_check();
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
