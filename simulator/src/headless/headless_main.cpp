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
 *     kamiframe-headless --verify-lua-pet
 *     kamiframe-headless --verify-pet-screen [--expect-checksum HEX]
 *     kamiframe-headless --verify-demand-curve
 *     kamiframe-headless --verify-screen-nav [--expect-checksum HEX]
 *     kamiframe-headless --verify-lua-creature
 *     kamiframe-headless --verify-assets
 *
 * Exit codes:
 *     0  everything asserted held
 *     1  a check failed
 */

#include "kf/app.h"
#include "kf/arena.h"
#include "kf/assets.h"
#include "kf/budget.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/hal/power.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"
#include "kf/pet.h"
#include "kf/rng.h"
#include "../host/host_storage.h"
#include "../host/host_time.h"
#include "../lvgl/kf_lvgl_port.h"
#include "../lvgl/kf_lvgl_proof_screen.h"
#include "../lvgl/kf_pet_screen.h"
#include "../lvgl/kf_screen_nav.h"
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
#include <vector>

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
     * itself. */
    constexpr uint32_t kFixedDtMs =
        static_cast<uint32_t>(KF_FRAME_BUDGET_US / 1000u);
    for (int i = 0; i < 30; ++i) {
        kf_pet_session_frame(kFixedDtMs);
        kf_screen_nav_frame();
        kf_lvgl_port_pump(kFixedDtMs);
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
        kf_screen_nav_frame();
        kf_lvgl_port_pump(kFixedDtMs);
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
 * same file kf_demo_init() loads "test_sprite" from), kf_assets_get() finds
 * it with the size and color key tools/kf_pack_assets.py --test-sprite
 * always writes, and -- the actual "matches what the packer wrote" proof --
 * its pixel bytes are byte-identical to the pack FILE's own bytes at the
 * offset ITS OWN directory entry names, read directly here with a small,
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
        check(sprite->width == 32u && sprite->height == 32u,
              "test_sprite is 32x32, the fixed size "
              "tools/kf_pack_assets.py --test-sprite always writes");
        check(sprite->has_color_key, "test_sprite carries a color key");
        check(sprite->pixels[0] == sprite->color_key,
              "the sprite's own corner pixel (outside the body ellipse in "
              "the generator's math) reads back as the color key -- a real "
              "decoded pixel, not zeroed or garbage memory");

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
                check(asset_type == 0u,
                      "the pack file's own directory marks this entry as "
                      "ASSET_TYPE_SPRITE (0)");

                uint16_t width = 0u, height = 0u, color_key = 0u;
                uint8_t has_color_key = 0u;
                std::memcpy(&width, entry + 36, sizeof(width));
                std::memcpy(&height, entry + 38, sizeof(height));
                std::memcpy(&color_key, entry + 40, sizeof(color_key));
                has_color_key = entry[42];
                check(width == sprite->width && height == sprite->height &&
                          color_key == sprite->color_key &&
                          (has_color_key != 0u) == sprite->has_color_key,
                      "the sprite metadata (width/height/color_key/"
                      "has_color_key) in the file's own type_meta matches "
                      "what kf_assets_get() reported");

                uint32_t data_offset = 0u;
                uint32_t data_bytes = 0u;
                std::memcpy(&data_offset, entry + 44, sizeof(data_offset));
                std::memcpy(&data_bytes, entry + 48, sizeof(data_bytes));

                check(static_cast<uint64_t>(data_offset) + data_bytes <=
                          raw.size(),
                      "the pixel data the directory names actually fits "
                      "inside the file");
                check(data_bytes == static_cast<uint32_t>(sprite->width) *
                                         sprite->height * 2u,
                      "data_bytes recorded in the file's own directory "
                      "matches width*height*2");

                if (static_cast<uint64_t>(data_offset) + data_bytes <=
                    raw.size()) {
                    check(std::memcmp(sprite->pixels, raw.data() + data_offset,
                                       data_bytes) == 0,
                          "the sprite kf_assets_get() handed back is "
                          "byte-identical to the pack file's own pixel "
                          "bytes at the offset its directory names -- the "
                          "loaded asset genuinely matches what the packer "
                          "wrote, not a stale, shifted, or byte-swapped "
                          "read");
                }
            }
        }
    }

    kf_assets_shutdown();

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
    bool verify_lua_pet = false;
    bool verify_pet_screen = false;
    bool verify_demand_curve = false;
    bool verify_screen_nav = false;
    bool verify_lua_creature = false;
    bool verify_assets = false;
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
                        "kamiframe-headless --verify-lua-pet\n"
                        "kamiframe-headless --verify-pet-screen "
                        "[--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-demand-curve\n"
                        "kamiframe-headless --verify-screen-nav "
                        "[--expect-checksum HEX]\n"
                        "kamiframe-headless --verify-lua-creature\n"
                        "kamiframe-headless --verify-assets\n");
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
