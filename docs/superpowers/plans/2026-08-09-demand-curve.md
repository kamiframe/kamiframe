# Per-Stage Demand Curve Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Status: COMPLETE

`pet_demand_curve_check` is registered in `simulator/CMakeLists.txt` and
passing.

**Goal:** Make need decay depend on life stage, so a baby demands attention every ~30 minutes and an adult every ~4 hours, replacing today's single uniform rate where hunger takes four real days to empty.

**Architecture:** `kf_pet_config` currently holds three flat `*_decay_mp_per_hour` fields. Replace them with a table indexed by `kf_pet_stage`, and have `apply_stage_segment()` look up the row for the pet's current stage. No new subsystem, no new file — the decay maths, the segmenting, and the offline fast-forward all stay exactly as they are. Only the number they read changes.

**Tech Stack:** C++17, no exceptions, no RTTI (ADR 0001). Core is heap-free (ADR 0008) — this adds a fixed-size array to an existing struct, no allocation. Desktop tests via ctest; device build via ESP-IDF v6.0.2.

## Global Constraints

- **HakoniwaOS is always one token.** Never bare "Hakoniwa" in prose, identifiers, or output.
- **Core stays heap-free.** `python3 tools/check_no_heap.py .` must keep reporting `core is heap-free`.
- **Zero warnings.** Desktop builds with `-DKAMIFRAME_WARNINGS_AS_ERRORS=ON`; ESP-IDF builds under `-Wall -Wextra -Werror`.
- **No libc string formatting in Core** — `kf/poison.h` bans it. This plan adds none.
- **All 13 existing ctest cases must pass at every commit**, except where a task explicitly updates a golden value and says why.
- Decay rates are `uint32_t` millipercent-per-hour. Full scale is 100000 mp = 100%.

---

## Numbers this plan implements

Derived from the spec's demand table. "Needs attention" is defined as
dropping to 70% (having lost 30,000 mp); "critical" is reaching 0.

| Stage | Hunger | Happiness | Energy | Empty after | Attention after |
|---|---|---|---|---|---|
| Egg | 0 | 0 | 0 | never | never |
| Baby | 66000 | 44000 | 33000 | ~91 min | ~27 min |
| Child | 33000 | 22000 | 16000 | ~3 h | ~55 min |
| Teen | 16000 | 11000 | 8000 | ~6.3 h | ~1.9 h |
| Adult | 8000 | 5500 | 4000 | ~12.5 h | ~3.8 h |

Hunger is fastest and energy slowest, preserving the 1 : 0.67 : 0.5 ratio
the current defaults already use.

---

### Task 1: Per-stage decay rates

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h` (the `kf_pet_config` struct and the stage enum block)
- Modify: `hakoniwaos/src/pet.cpp` (`kf_pet_default_config()`, `apply_stage_segment()`)
- Test: `simulator/src/headless/headless_main.cpp` (new check function, wired into `main()`)

**Interfaces:**
- Consumes: `kf_pet_stage` (existing enum, `KF_PET_STAGE_EGG`=0 .. `KF_PET_STAGE_ADULT`=4); `kf_pet_config`; `apply_stage_segment(kf_pet_state*, const kf_pet_config*, uint32_t segment)`.
- Produces: `KF_PET_STAGE_COUNT` (== 5u); `kf_pet_stage_rates` struct; `kf_pet_config::stage_rates[KF_PET_STAGE_COUNT]`. Later plans (variations, mess) read `stage_rates` to scale their own effects by stage.

- [ ] **Step 1: Add the stage count constant**

In `hakoniwaos/include/kf/pet.h`, immediately after the `kf_pet_stage` enum
closing brace (currently line 84):

```c
/* Number of life stages, for sizing per-stage tables. Kept next to the enum
 * so the two cannot drift: adding a stage without widening the tables that
 * index by it would read past the end of every one of them. */
#define KF_PET_STAGE_COUNT 5u
```

- [ ] **Step 2: Add the per-stage rate struct and replace the flat fields**

In `hakoniwaos/include/kf/pet.h`, add above `kf_pet_config`:

```c
/* Decay rates for one life stage, in millipercent per hour.
 *
 * Per-stage rather than one set for the whole life because demand IS the
 * game: a baby that needs attention every half hour and an adult you check
 * on a few times a day are the same creature at different points, and a
 * single rate cannot express both. See
 * docs/superpowers/specs/2026-08-09-core-care-loop-design.md section 2. */
typedef struct {
    uint32_t hunger_mp_per_hour;
    uint32_t happiness_mp_per_hour;
    uint32_t energy_mp_per_hour;
} kf_pet_stage_rates;
```

Then in `kf_pet_config`, delete these three lines:

```c
    uint32_t hunger_decay_mp_per_hour;
    uint32_t happiness_decay_mp_per_hour;
    uint32_t energy_decay_mp_per_hour;
```

and replace them with:

```c
    /* Indexed by kf_pet_stage. The EGG row is all zeroes and is never read
     * -- apply_stage_segment() returns early for eggs -- but it is present
     * so the table can be indexed by stage without an offset, which is one
     * fewer thing to get wrong. */
    kf_pet_stage_rates stage_rates[KF_PET_STAGE_COUNT];
```

- [ ] **Step 3: Write the failing test**

In `simulator/src/headless/headless_main.cpp`, add this function immediately
before `run_pet_screen_check`:

```cpp
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
    return ok ? 0 : 1;
}
```

`apply_stage_segment()` is file-static in `pet.cpp`. Expose it for testing by
adding this declaration to `hakoniwaos/include/kf/pet.h`, after
`kf_pet_advance()`:

```c
/* Test seam: applies exactly one decay segment at the pet's CURRENT stage,
 * without the stage-transition logic kf_pet_advance() wraps around it.
 * Exists so a test can compare two stages over identical elapsed time
 * without constructing two whole life histories. Not for gameplay use --
 * kf_pet_advance() is the real entry point. */
void apply_stage_segment_for_test(kf_pet_state *state,
                                   const kf_pet_config *config,
                                   uint32_t segment_seconds);
```

Wire the check into `main()` in `headless_main.cpp`: add
`bool verify_demand_curve = false;` beside the other `verify_*` flags, add
the `--verify-demand-curve` branch to the argument loop matching the style of
`--verify-pet-screen`, add the line to the usage text, and add this beside
the other dispatches:

```cpp
    if (verify_demand_curve) {
        return run_pet_demand_curve_check();
    }
```

Register it in `simulator/CMakeLists.txt` beside the other `add_test` calls:

```cmake
add_test(NAME pet_demand_curve_check
         COMMAND kamiframe-headless --verify-demand-curve)
```

- [ ] **Step 4: Run the test and watch it fail to compile**

```bash
cmake --build build-desktop --target kamiframe-headless --parallel
```

Expected: FAILS to compile, with errors naming `stage_rates` as having no
member `hunger_mp_per_hour` in `kf_pet_default_config()`, and
`apply_stage_segment_for_test` undefined. That is the correct failure — the
config has been widened but nothing fills or reads it yet.

- [ ] **Step 5: Fill in the default rates**

In `hakoniwaos/src/pet.cpp`, inside `kf_pet_default_config()`, delete the
three lines assigning `c.hunger_decay_mp_per_hour`,
`c.happiness_decay_mp_per_hour` and `c.energy_decay_mp_per_hour`, and put
this in their place:

```cpp
    /* Per-stage demand. Derived in
     * docs/superpowers/plans/2026-08-09-demand-curve.md: "needs attention"
     * is dropping to 70%, "critical" is reaching zero, and the three needs
     * keep the 1 : 0.67 : 0.5 ratio the old flat defaults used, so hunger
     * bites first and energy last.
     *
     * These are the numbers most likely to be wrong after a week of living
     * with a real pet. They are here, together, in one table, precisely so
     * that tuning them is editing five rows rather than hunting through
     * logic. */
    c.stage_rates[KF_PET_STAGE_EGG] = {0u, 0u, 0u};          /* never decays */
    c.stage_rates[KF_PET_STAGE_BABY] = {66000u, 44000u, 33000u};  /* ~30 min */
    c.stage_rates[KF_PET_STAGE_CHILD] = {33000u, 22000u, 16000u}; /* ~1 h */
    c.stage_rates[KF_PET_STAGE_TEEN] = {16000u, 11000u, 8000u};   /* ~2 h */
    c.stage_rates[KF_PET_STAGE_ADULT] = {8000u, 5500u, 4000u};    /* ~4 h */
```

- [ ] **Step 6: Look up the rates by stage**

In `hakoniwaos/src/pet.cpp`, inside `apply_stage_segment()`, immediately
after the `KF_PET_STAGE_EGG` early return, add:

```cpp
    /* Indexed directly by stage -- the enum's values are 0..4 by definition
     * (kf/pet.h) and KF_PET_STAGE_COUNT sits next to it to keep them in
     * step. Clamped anyway: a corrupted save that survived the version
     * check should degrade to the gentlest rates, not read off the end of
     * the table. */
    unsigned stage_index = static_cast<unsigned>(state->stage);
    if (stage_index >= KF_PET_STAGE_COUNT) {
        stage_index = KF_PET_STAGE_ADULT;
    }
    const kf_pet_stage_rates &rates = config->stage_rates[stage_index];
```

Then replace the three `apply_decay(...)` calls' rate arguments so they read
from `rates`:

```cpp
        apply_decay(state->hunger_mp, rates.hunger_mp_per_hour, segment);
```

and the equivalent for happiness (`rates.happiness_mp_per_hour`) and energy
(`rates.energy_mp_per_hour`).

- [ ] **Step 7: Add the test seam**

At the end of `hakoniwaos/src/pet.cpp`, outside the anonymous namespace:

```cpp
void apply_stage_segment_for_test(kf_pet_state *state,
                                   const kf_pet_config *config,
                                   uint32_t segment_seconds) {
    apply_stage_segment(state, config, segment_seconds);
}
```

- [ ] **Step 8: Run the new test**

```bash
cmake --build build-desktop --target kamiframe-headless --parallel && ./build-desktop/kamiframe-headless --verify-demand-curve
```

Expected: `PASS`, and a log line showing baby hunger well below 40000 and
adult hunger above 90000.

- [ ] **Step 9: Run the whole suite and expect breakage**

```bash
ctest --test-dir build-desktop --output-on-failure
```

Expected: `pet_offline_ageing_check` and/or `pet_stage_evolution_check` FAIL.
They assert against the old flat rates. Read each failure, confirm the new
value is what the new rates predict, and update the expected constant in
`headless_main.cpp` — do NOT loosen the assertion to make it pass. If a
failure is not explained by the rate change, stop: something else broke.

- [ ] **Step 10: Verify the heap rule and the device build**

```bash
python3 tools/check_no_heap.py .
```

Expected: `check_no_heap: core is heap-free`.

```bash
cd ports/esp32 && source ~/esp/esp-idf/export.sh && idf.py build
```

Expected: builds clean, zero warnings. The config struct grew by 60 bytes
(5 stages x 3 uint32_t) minus the 12 bytes removed — a static, not heap,
change.

- [ ] **Step 11: Commit**

```bash
git add hakoniwaos/include/kf/pet.h hakoniwaos/src/pet.cpp \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "Per-stage demand: a baby needs care every ~30 min, an adult every ~4 h"
```

---

### Task 2: Prove it on hardware

**Files:**
- No code changes. This task is verification.

**Interfaces:**
- Consumes: Task 1's `stage_rates`; the existing KFDBG bridge (`kf_debug.py advance` / `state`).
- Produces: nothing. A confirmation, and a tuning decision.

- [ ] **Step 1: Flash the new firmware**

```bash
cd ports/esp32 && source ~/esp/esp-idf/export.sh && idf.py build flash
```

- [ ] **Step 2: Hatch the egg and read the baby's rate**

Quit any serial monitor first — it holds the port.

```bash
python3 tools/kf_debug.py reset && python3 tools/kf_debug.py advance 1h && python3 tools/kf_debug.py state
```

Expected: the pet is a BABY (the egg lasts one hour), and its needs have
begun dropping rather than sitting at 100%.

- [ ] **Step 3: Confirm the baby's demand is real**

```bash
python3 tools/kf_debug.py advance 30m && python3 tools/kf_debug.py state
```

Expected: hunger has fallen by roughly 30% of full scale (about 33000 mp)
from where it was. This is the number the whole design rests on — if it feels
wrong when you watch it in real time rather than by skipping, that is a
tuning result worth recording, not a bug.

- [ ] **Step 4: Confirm an adult is gentler**

```bash
python3 tools/kf_debug.py reset && python3 tools/kf_debug.py advance 1w && python3 tools/kf_debug.py state
```

Expected: an ADULT, and its needs drop visibly more slowly per hour than the
baby's did. Skipping a week will have driven needs to zero and probably
triggered a stage path shaped by total neglect — that is correct behaviour,
not a fault.

- [ ] **Step 5: Record the outcome**

If the rates feel right, note it in the plan and move on. If they do not,
change only the numbers in `kf_pet_default_config()` — the whole point of the
table is that tuning touches one place — re-run `ctest`, and commit
separately with a message saying what felt wrong and what changed.

---

## What this plan deliberately does NOT do

Named so the next plan's author does not have to guess where the boundary is:

- **Sleep.** A baby at these rates cannot survive a night unattended. That is
  known and is the next plan — scheduled sleep on real wall-clock time, per
  the spec's section 3. Do not add a nap mechanic here.
- **Care variations and preferences.** The spec's section 5. Care actions keep
  their current signatures.
- **Mess, sickness and death.** Sections 4 and 7.
- **Any screen change.** The bars will simply move faster.
