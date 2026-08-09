# Sickness and Death Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sustained neglect makes the creature sick, sickness compounds, and if it is never addressed the creature dies — with a long, readable warning first.

**Architecture:** One accumulator, `neglect_seconds`, drives all three states. It rises while the creature is in a neglected condition and falls at the same rate while it is not. Crossing `sickness_onset_seconds` sets `sick`; returning to zero clears it (hysteresis, so a single button press cures nothing); reaching `sickness_death_seconds` kills. The screen and scripts read the same accumulator to escalate distress between those two thresholds. It all happens inside `apply_stage_segment()` alongside the needs and mess, so offline fast-forward is covered by the code path that already exists rather than a second one.

**Tech Stack:** C++17, no exceptions/RTTI, heap-free core (`tools/check_no_heap.py`). Tests are subcommands of the headless simulator binary, registered in `simulator/CMakeLists.txt` and run by ctest.

## Global Constraints

- **Core stays heap-free.** No `new`, `malloc`, `std::vector`, `std::string`. `python3 tools/check_no_heap.py .` must print `core is heap-free`.
- **Integer maths only, millipercent (0..100000).** No floating point anywhere in `pet.cpp`. Every intermediate product goes through `uint64_t` before being clamped back.
- **Closed-form, never per-second.** No loop in `pet.cpp` may be bounded by `elapsed_seconds`. Offline fast-forward hands this code weeks at a time.
- **Warnings are errors:** `cmake -S . -B build -DKAMIFRAME_WARNINGS_AS_ERRORS=ON`. Both GCC and MSVC build this.
- **The device build must keep building.** `ports/esp32` compiles the same core.
- **Save format is packed byte by byte** in `pack()`/`unpack()`. A new field means bumping `kSaveVersion` and `KF_PET_SAVE_BYTES` together.
- **Comments explain why, not what.** Match the density and voice of the surrounding file.

### Current signatures — check these before writing any test

They changed this week. Getting them wrong is the fastest way to waste a build:

```cpp
void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config);  /* takes config */
void kf_pet_play(kf_pet_state *state);                               /* does not */
void kf_pet_rest(kf_pet_state *state);                               /* does not */
void kf_pet_clean(kf_pet_state *state);                              /* does not */
void apply_stage_segment_for_test(kf_pet_state *state,
                                   const kf_pet_config *config,
                                   uint32_t segment_seconds);
```

---

## Design decisions made here, and what they cost

Read this section before Task 1. All four are load bearing.

### CORRECTION, after implementation: both-ends sampling was wrong

**The rule described immediately below shipped and was then replaced.** It is left here because the reasoning that led to it is worth seeing alongside what was wrong with it.

Sampling both ends and splitting the difference underestimates badly, for a reason the note below misses entirely: the neglect condition is a *threshold* on a value that decays, not a linear quantity in its own right. A creature left in a drawer for a day starts full and ends empty, so the trapezoid credits half a day — but the needs actually ran out about three hours in, so five sixths of that day was neglect. In practice a cared-for creature could be abandoned for a full day and come back perfectly healthy, which is the exact scenario the feature exists for.

What shipped instead solves for the crossing point exactly. The needs decay linearly at a known rate, so the moment a need falls below the threshold is one division, not a search. Three cases: already neglected at the segment's start credits all of it (exact — nothing recovers inside a segment, since care only happens between them); the needs running out partway credits from that moment; and mess alone going critical falls back to half the segment, the one estimate left, because dirtiness has no closed-form crossing when its rate steps up with every poop.

See `seconds_until_need_neglect()` in `pet.cpp` for the implementation and the full reasoning.

### The superseded rule: the neglect condition is sampled at BOTH ends of a segment

`apply_stage_segment()` receives a segment that may be one frame or a fortnight. The care integral samples the needs at the *start* of its segment and documents that as a deliberate left-Riemann approximation.

Sickness cannot do that. A device switched off for a week begins that segment with a perfectly healthy creature, so a start-only sample would conclude the week was fine — the single most important case for this feature, silently wrong. An end-only sample has the opposite failure: a fortnight that only turned bad in its final hour would be credited as a fortnight of neglect.

So each end counts for half the segment. A segment that begins and ends neglected credits all of it; one that begins fine and ends badly credits half and refunds half, netting zero; one that is fine at both ends refunds all of it. This is the trapezoid rule, it is two comparisons and no loop, and its error is bounded by the segment length rather than growing with it.

**Consequence the tests depend on:** a single long segment that starts healthy nets *zero* neglect. Driving a creature to illness always takes at least two calls — one to make things bad, and one that is bad at both ends. Every test below is written that way, and it is not incidental.

### A creature that has never been cared for does not get sick

`neglect_seconds` only moves when `care_actions_taken > 0`.

This is not a convenience. The character bible's Hokorimaru — the dust form — is defined as what a creature becomes when it is *never interacted with at all*, and reaching it takes a full childhood. If total neglect killed the creature within a day, a documented character would be unobtainable. Abandonment turns a creature to dust; it is the creature that has known care and then lost it that sickens.

**Flag this in the completion report.** It is a behaviour decision, not a technical one, and Chris may want the opposite. The alternative — Hokorimaru is reached and then dies shortly after — was rejected because a form you cannot keep is not a form.

### Recovery is exactly as slow as the damage

The accumulator falls at the same rate it rises, so three hours of neglect takes three hours of attentive care to undo, and illness only lifts when it reaches zero.

This is what "cure by care, not a medicine button" has to mean to be real. If care cleared it outright, the optimal play would be to ignore the creature until it is ill and then press four buttons.

### `sickness_death_seconds == 0` means the creature cannot die

A zero sentinel, the same shape `poop_interval_seconds == 0` already uses for "no mess".

Several current checks raise a creature across days or a month with little or no care in order to measure something else — the evolution branches, the personality accumulators, the dust form. Under real rules most of those creatures are dead. Rather than distort them into creatures receiving periodic token care, they set this to 0 and say why.

It is also a real product lever: an off switch for permanent death, which Chris may want for a gentler mode, obtained for free rather than designed later.

---

## File Structure

| File | What changes |
|---|---|
| `hakoniwaos/include/kf/pet.h` | Three new `kf_pet_state` fields, seven new `kf_pet_config` fields, save version and size |
| `hakoniwaos/src/pet.cpp` | Neglect/sickness/death in `apply_stage_segment()`; death gating in `kf_pet_advance()` and the four care actions; `pack()`/`unpack()`; `kf_pet_default_config()`; `kf_pet_init()` |
| `simulator/src/headless/headless_main.cpp` | Two new checks, one new Lua proof stage; existing long-horizon checks get an immortal config |
| `simulator/src/lua/kf_lua_port.cpp` | `pet.sick()`, `pet.dead()`, `pet.neglect_seconds()` |
| `simulator/src/lua/kf_lua_pet_proof_script.h` | One more proof script |
| `simulator/CMakeLists.txt` | Two `add_test()` entries |

---

## Task 1: The neglect accumulator, and sickness

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp`
- Modify: `simulator/src/headless/headless_main.cpp`
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf_pet_state::dirtiness_mp`, `poop_count`, `care_actions_taken`, `apply_stage_segment_for_test()`
- Produces: `kf_pet_state::neglect_seconds` (`uint32_t`), `kf_pet_state::sick` (`bool`); config `neglect_need_mp`, `neglect_dirtiness_mp`, `neglect_poop_count`, `sickness_onset_seconds`; save version 6

- [ ] **Step 1: Write the failing test**

Add to `simulator/src/headless/headless_main.cpp`, immediately after `run_pet_dirtiness_check()`:

```cpp
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
    kf_pet_feed(&pet, &config);
    check(!pet.sick, "a fresh creature is not sick");
    check(pet.neglect_seconds == 0u, "and has accumulated no neglect");

    /* Four hours: healthy at the start, hungry and messy by the end. Half
     * credited, half refunded -- nets zero, by design. */
    apply_stage_segment_for_test(&pet, &config, 4u * 3600u);
    check(pet.neglect_seconds == 0u,
          "a stretch that only goes bad at the end nets no neglect");

    /* Two more hours, bad at both ends this time. */
    apply_stage_segment_for_test(&pet, &config, 2u * 3600u);
    check(pet.neglect_seconds > 0u,
          "neglect accumulates once things are already bad");
    check(!pet.sick, "but two hours of it is not yet illness");

    /* Past the onset threshold. */
    apply_stage_segment_for_test(&pet, &config, config.sickness_onset_seconds);
    check(pet.sick, "sustained neglect makes the creature sick");

    /* One round of every button does not undo it. That is the whole point
     * of curing through care rather than through a medicine action. */
    kf_pet_feed(&pet, &config);
    kf_pet_play(&pet);
    kf_pet_rest(&pet);
    kf_pet_clean(&pet);
    check(pet.sick, "a single round of care does not cure it on the spot");

    /* Sustained care does. Sixty ten-minute stretches of being properly
     * looked after -- comfortably more than the accumulated neglect, since
     * what is being checked is that recovery HAPPENS, not the exact rate. */
    for (int i = 0; i < 60; ++i) {
        kf_pet_feed(&pet, &config);
        kf_pet_play(&pet);
        kf_pet_rest(&pet);
        kf_pet_clean(&pet);
        apply_stage_segment_for_test(&pet, &config, 600u);
    }
    check(pet.neglect_seconds == 0u, "attentive care works the clock back down");
    check(!pet.sick, "and the creature recovers");

    /* Filth alone is enough, with every need full -- mess is a real neglect
     * channel, not decoration on top of the three bars. */
    kf_pet_state filthy{};
    kf_pet_init(&filthy);
    filthy.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&filthy, &config);
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
    check(untouched.neglect_seconds == 0u && !untouched.sick,
          "a creature that has never known care does not sicken from its "
          "absence -- it is on the dust path instead");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
```

Register the subcommand in `main()` beside the existing ones — find the `--pet-dirtiness-check` dispatch and copy its shape:

```cpp
    if (mode == "--pet-sickness-check") {
        return run_pet_sickness_check();
    }
```

In `simulator/CMakeLists.txt`, beside the `pet_dirtiness_check` entry:

```cmake
add_test(NAME pet_sickness_check
         COMMAND kamiframe-headless --pet-sickness-check)
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cmake --build build -j8
```

Expected: compile failure — `kf_pet_state` has no member `sick`.

- [ ] **Step 3: Add the state and config fields**

In `hakoniwaos/include/kf/pet.h`, inside `kf_pet_config`, directly after the four mess fields:

```cpp
    /* What counts as neglect. A need at or below neglect_need_mp, more than
     * neglect_poop_count poops waiting, or dirtiness at or above
     * neglect_dirtiness_mp -- any one of the three is enough.
     *
     * Three separate channels rather than one blended score, because the
     * player has to be able to work out WHICH thing they are getting wrong,
     * and a blend of "somewhat hungry and somewhat filthy" tells them
     * nothing they can act on. */
    kf_pet_millipercent neglect_need_mp;
    kf_pet_millipercent neglect_dirtiness_mp;
    uint8_t neglect_poop_count;

    /* How much accumulated neglect turns into sickness. The accumulator
     * falls again at the same rate while the creature is looked after, so
     * this doubles as how long attentive care takes to cure it. */
    uint32_t sickness_onset_seconds;
```

In `kf_pet_state`, directly after `dirtiness_mp`:

```cpp
    /* Seconds of accumulated neglect. Rises while the creature is in a
     * neglected condition and falls at the same rate while it is not -- so
     * one press of every button does not wipe out a day of damage, and a
     * creature that has been badly treated takes proportionally longer to
     * nurse back.
     *
     * ONE accumulator drives all three states: illness at
     * sickness_onset_seconds, death at sickness_death_seconds, and the
     * escalating distress the screen shows between them. Three separate
     * timers would have to be kept consistent with each other; this cannot
     * disagree with itself. */
    uint32_t neglect_seconds;

    /* Whether the creature is currently ill. Stored rather than derived
     * from neglect_seconds, because the thresholds are asymmetric on
     * purpose: it falls ill at sickness_onset_seconds but only recovers at
     * zero. That hysteresis is what stops a creature hovering at the
     * threshold flickering in and out of illness every frame, and it
     * cannot be recomputed from the accumulator alone. */
    bool sick;
```

- [ ] **Step 4: Add the defaults**

In `kf_pet_default_config()`, after the dirtiness defaults:

```cpp
    /* A need at 10% or below, six poops down, or dirtiness past the stink
     * threshold. The dirtiness figure is deliberately the same place the
     * stink lines appear (KF_PET_DIRTY_STINK_MP): what the player can see
     * is what is hurting the creature, rather than an invisible second
     * threshold they would have to infer. */
    c.neglect_need_mp = 10000u;
    c.neglect_dirtiness_mp = KF_PET_DIRTY_STINK_MP;
    c.neglect_poop_count = 6u;

    /* Three hours of neglect to fall ill. Tuning number: long enough that
     * an afternoon out cannot do it from full bars, short enough that a
     * neglected creature shows it the same day. */
    c.sickness_onset_seconds = 3u * 3600u;
```

In `kf_pet_init()`, after the mess fields:

```cpp
    state->neglect_seconds = 0u;
    state->sick = false;
```

- [ ] **Step 5: Implement the accumulator**

In `hakoniwaos/src/pet.cpp`, add above `apply_stage_segment()`:

```cpp
/* Whether the creature is in a neglected condition right now. A pure read
 * of the state, deliberately: nothing is stored, so there is no second copy
 * of "is it neglected" that can drift out of step with the fields it comes
 * from. */
bool is_neglected(const kf_pet_state *state, const kf_pet_config *config) {
    return state->hunger_mp <= config->neglect_need_mp ||
           state->happiness_mp <= config->neglect_need_mp ||
           state->energy_mp <= config->neglect_need_mp ||
           state->poop_count > config->neglect_poop_count ||
           state->dirtiness_mp >= config->neglect_dirtiness_mp;
}

/* Adds `add` to a saturating uint32_t counter. */
uint32_t saturating_add_u32(uint32_t value, uint32_t add) {
    const uint64_t sum = static_cast<uint64_t>(value) + add;
    return sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}
```

Inside `apply_stage_segment()`, capture the "before" reading alongside the existing `hunger_before_mp` block (it must be read before any decay is applied):

```cpp
    const bool neglected_before = is_neglected(state, config);
```

Then at the very end of the function, after the `accumulate_personality()` call:

```cpp
    /* Neglect, and the illness it turns into. Evaluated LAST in the
     * segment, and sampled at BOTH ends of it.
     *
     * Both ends, because neither alone survives offline fast-forward: a
     * week in a drawer begins with a healthy creature, so sampling only the
     * start would call that week fine, and sampling only the end would call
     * a fortnight that soured in its final hour a fortnight of neglect.
     * Each end counting for half is the trapezoid rule -- two comparisons,
     * no loop, and an error bounded by the segment rather than growing with
     * it. A segment that starts healthy and ends badly therefore nets
     * exactly zero, which is the correct reading of "half of it was fine".
     *
     * A creature that has never been cared for is exempt from all of it.
     * The character bible's dust form is what total absence of interaction
     * produces, and it takes a full childhood to reach; illness would kill
     * that creature days before it got there. It is the creature that has
     * known care and then lost it that sickens. */
    if (state->care_actions_taken > 0u) {
        const bool neglected_after = is_neglected(state, config);

        uint32_t neglected_for = 0u;
        if (neglected_before) {
            neglected_for += segment / 2u;
        }
        if (neglected_after) {
            neglected_for += segment - (segment / 2u);
        }
        const uint32_t cared_for = segment - neglected_for;

        state->neglect_seconds =
            saturating_add_u32(state->neglect_seconds, neglected_for);
        state->neglect_seconds = state->neglect_seconds > cared_for
                                      ? state->neglect_seconds - cared_for
                                      : 0u;

        if (state->neglect_seconds >= config->sickness_onset_seconds) {
            state->sick = true;
        } else if (state->neglect_seconds == 0u) {
            state->sick = false;
        }
    }
```

- [ ] **Step 6: Bump the save format**

In `pet.cpp`, `kSaveVersion` becomes `6`. Extend its comment in the same style the version-5 entry uses: what was added, and why an older save cannot simply be read with the new fields defaulted.

In `pack()`, after `put_u32(out, off, state->dirtiness_mp);`:

```cpp
    put_u32(out, off, state->neglect_seconds);
    put_u8(out, off, state->sick ? 1u : 0u);
```

In `unpack()`, at the matching position:

```cpp
    state->neglect_seconds = get_u32(in, off);
    state->sick = get_u8(in, off) != 0u;
```

In `pet.h`, `KF_PET_SAVE_BYTES` becomes `88u` (83 + 4 + 1), and its comment gains the version-6 entry.

- [ ] **Step 7: Run the new test**

```bash
cmake --build build -j8 && ctest --test-dir build -R pet_sickness_check --output-on-failure
```

Expected: PASS.

If a threshold assertion fails, **do not adjust the test's numbers to fit**. Work out which side is wrong first: the sequence above was derived from the default config's child-stage rates (hunger 33000 mp/hour, so four hours empties the bar), and if those defaults have since changed, the honest fix is to make the test derive its segment lengths from the config rather than to nudge a constant until it goes green.

- [ ] **Step 8: Run the whole suite**

```bash
ctest --test-dir build --output-on-failure
```

Nothing should break yet — this task adds an accumulator that nothing reads. If something does break, the accumulator is wrong; investigate it rather than the test.

- [ ] **Step 9: Verify the constraints**

```bash
python3 tools/check_no_heap.py .
```

```bash
cd ports/esp32 && idf.py build
```

- [ ] **Step 10: Commit**

```bash
git add -A && git commit -m "Neglect accumulates, and enough of it makes the creature ill"
```

---

## Task 2: What sickness does

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp`
- Modify: `simulator/src/headless/headless_main.cpp`

**Interfaces:**
- Consumes: `kf_pet_state::sick` from Task 1
- Produces: config `sick_decay_multiplier_percent`, `sick_happiness_drain_mp_per_hour`

- [ ] **Step 1: Write the failing test**

Append to `run_pet_sickness_check()`, immediately before its final `std::printf`:

```cpp
    /* Illness compounds. Two creatures, same stage, same full needs, same
     * elapsed time -- the only difference between them is that one is ill.
     * Both are fed once first, because an untouched creature would be
     * exempt from the accumulator and drift out of the comparison. */
    kf_pet_state well{};
    kf_pet_init(&well);
    well.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&well, &config);

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
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cmake --build build -j8
```

Expected: compile failure — no member `sick_decay_multiplier_percent`.

- [ ] **Step 3: Add the config fields and defaults**

In `kf_pet_config`, after `sickness_onset_seconds`:

```cpp
    /* What being ill costs. The multiplier is a percentage applied to every
     * decay rate (100 = unchanged); the drain is happiness lost on top of
     * it, independent of the stage's own happiness rate.
     *
     * Two levers rather than one, because they do different jobs: the
     * multiplier makes an ill creature harder to keep out of the neglected
     * condition at all, and the drain makes it visibly miserable even while
     * the player is keeping every bar topped up. Together they turn
     * "ignored for an afternoon" into a spiral instead of a plateau. */
    uint32_t sick_decay_multiplier_percent;
    uint32_t sick_happiness_drain_mp_per_hour;
```

In `kf_pet_default_config()`:

```cpp
    /* Twice the decay, and about a fifth of the happiness bar an hour on
     * top. Tuning numbers. */
    c.sick_decay_multiplier_percent = 200u;
    c.sick_happiness_drain_mp_per_hour = 20000u;
```

- [ ] **Step 4: Apply them**

Add above `apply_stage_segment()`:

```cpp
/* Scales a decay rate by the sickness multiplier. uint64_t intermediate for
 * the reason this file's header comment gives: no product here is computed
 * in uint32_t and hoped for. */
uint32_t sick_scaled_rate(uint32_t rate_mp_per_hour, bool sick,
                           const kf_pet_config *config) {
    if (!sick) {
        return rate_mp_per_hour;
    }
    const uint64_t scaled = static_cast<uint64_t>(rate_mp_per_hour) *
                             config->sick_decay_multiplier_percent / 100ull;
    return scaled > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(scaled);
}
```

Replace the three `apply_decay()` calls in `apply_stage_segment()` with:

```cpp
    state->hunger_mp = apply_decay(
        state->hunger_mp,
        sick_scaled_rate(rates.hunger_mp_per_hour, state->sick, config),
        segment);
    state->happiness_mp = apply_decay(
        state->happiness_mp,
        sick_scaled_rate(rates.happiness_mp_per_hour, state->sick, config),
        segment);
    state->energy_mp = apply_decay(
        state->energy_mp,
        sick_scaled_rate(rates.energy_mp_per_hour, state->sick, config),
        segment);

    /* The extra happiness drain, on top of the scaled decay above. Routed
     * through apply_decay() rather than done by hand so it clamps at zero
     * the same way everything else does. */
    if (state->sick) {
        state->happiness_mp =
            apply_decay(state->happiness_mp,
                        config->sick_happiness_drain_mp_per_hour, segment);
    }
```

**Ordering trap:** `hunger_before_mp`, `happiness_before_mp`, `energy_before_mp` and `neglected_before` are captured *above* this for the care integral and the accumulator. They must stay where they are — moving them would silently change what the care integral measures.

- [ ] **Step 5: Run the test**

```bash
cmake --build build -j8 && ctest --test-dir build -R pet_sickness_check --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Run the whole suite**

```bash
ctest --test-dir build --output-on-failure
```

Decay rates now change mid-life, so a long-horizon check may shift. Read each failure honestly: a check that measures branch selection or personality should not care, and if it does, say so in the report rather than quietly relaxing its assertion.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "Illness compounds: faster decay and a happiness drain on top"
```

---

## Task 3: Death, and the script bindings

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp`
- Modify: `simulator/src/headless/headless_main.cpp`
- Modify: `simulator/src/lua/kf_lua_port.cpp`
- Modify: `simulator/src/lua/kf_lua_pet_proof_script.h`
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf_pet_state::neglect_seconds`, `sick`
- Produces: `kf_pet_state::dead` (`bool`); config `sickness_death_seconds`; Lua `pet.sick()`, `pet.dead()`, `pet.neglect_seconds()`; save version 7

- [ ] **Step 1: Write the failing test**

Add after `run_pet_sickness_check()`:

```cpp
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
    kf_pet_feed(&pet, &config);

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
    kf_pet_feed(&pet, &config);
    kf_pet_play(&pet);
    kf_pet_rest(&pet);
    kf_pet_clean(&pet);
    check(pet.dead, "a week of care does not bring it back");
    check(pet.hunger_mp == hunger_at_death, "nothing decays after death");
    check(pet.stage == stage_at_death, "and it does not keep growing up");
    check(pet.care_actions_taken == care_at_death,
          "care aimed at a dead creature does not count as care -- it must "
          "not be able to move it off the dust path posthumously");

    /* The off switch: zero means it cannot die, the same sentinel shape
     * poop_interval_seconds == 0 already uses for "no mess". */
    kf_pet_config immortal = kf_pet_default_config();
    immortal.sickness_death_seconds = 0u;

    kf_pet_state survivor{};
    kf_pet_init(&survivor);
    survivor.stage = KF_PET_STAGE_CHILD;
    kf_pet_feed(&survivor, &immortal);
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
    check(!untouched.dead,
          "a creature that has never been touched does not die of it -- it "
          "is on the dust path, which takes a whole childhood to walk");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
```

Register `--pet-death-check` in `main()`, and:

```cmake
add_test(NAME pet_death_check
         COMMAND kamiframe-headless --pet-death-check)
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cmake --build build -j8
```

Expected: compile failure — no member `dead`.

- [ ] **Step 3: Add the field, the config and the defaults**

In `kf_pet_state`, after `sick`:

```cpp
    /* Whether this creature has died. Terminal: nothing in this file ever
     * clears it, and there is no revival action anywhere, by design. A new
     * creature is a new kf_pet_init(), which is the honest way to say what
     * has happened. */
    bool dead;
```

In `kf_pet_config`, after `sick_happiness_drain_mp_per_hour`:

```cpp
    /* Accumulated neglect at which the creature dies. ZERO MEANS IT NEVER
     * DOES -- the same sentinel shape poop_interval_seconds == 0 uses for
     * "no mess". That is an off switch for permanent death, a product
     * decision Chris may want for a gentler mode, and it is also what lets
     * a check raise a badly-treated creature to adulthood without modelling
     * the token care it would otherwise need to survive. */
    uint32_t sickness_death_seconds;
```

Default:

```cpp
    /* A full day of accumulated neglect, on top of the three hours that
     * made it ill -- twenty-one hours of visible, escalating distress
     * before the end. Tuning number, and the one here most worth being
     * generous with: every death should feel deserved. */
    c.sickness_death_seconds = 86400u;
```

`kf_pet_init()` gains `state->dead = false;`.

- [ ] **Step 4: Implement death**

At the top of `apply_stage_segment()`, beside the egg exemption:

```cpp
    if (state->dead) {
        /* Nothing decays, nothing accumulates, no mess arrives. The same
         * shape as the egg exemption below and for a comparable reason:
         * there is no simulation left to run. */
        return;
    }
```

In the neglect block from Task 1, after the two sickness thresholds:

```cpp
        if (config->sickness_death_seconds > 0u) {
            if (state->neglect_seconds > config->sickness_death_seconds) {
                /* Capped, so an abandoned creature's counter cannot drift
                 * off toward saturation and take a correspondingly absurd
                 * amount of care to walk back if it is somehow revived by a
                 * future feature. */
                state->neglect_seconds = config->sickness_death_seconds;
            }
            if (state->neglect_seconds >= config->sickness_death_seconds) {
                state->dead = true;
            }
        }
```

At the top of `kf_pet_advance()`:

```cpp
    if (state->dead) {
        return;
    }
```

As the first statement of each of `kf_pet_feed()`, `kf_pet_play()`, `kf_pet_rest()` and `kf_pet_clean()`:

```cpp
    if (state->dead) {
        return;
    }
```

**This must come before the `care_actions_taken` increment** in each one, or feeding a dead creature would still count as care.

- [ ] **Step 5: Extend the save format**

`kSaveVersion` becomes `7` — a second bump in the same branch is correct and cheap; a save written between Task 1 and Task 3 is a real file on a real developer's disk with a genuinely different layout, and versions exist precisely so that is refused rather than misread.

In `pack()`, after the `sick` byte:

```cpp
    put_u8(out, off, state->dead ? 1u : 0u);
```

In `unpack()`, at the matching position:

```cpp
    state->dead = get_u8(in, off) != 0u;
```

`KF_PET_SAVE_BYTES` becomes `89u`. Extend both version comments.

- [ ] **Step 6: Run the test**

```bash
cmake --build build -j8 && ctest --test-dir build -R pet_death_check --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Run the whole suite — this is where the long-horizon checks break**

```bash
ctest --test-dir build --output-on-failure
```

Expect real failures. `run_hokorimaru_check()`, the evolution-tree checks and the personality check all raise creatures across days or a month. Any whose creature has been cared for even once and then left alone will now die partway through and stop progressing.

For each failure the fix is a **config with `sickness_death_seconds = 0`**, and a comment saying what it protects: that this check is about branch selection (or personality, or the dust form), not about survival, and that modelling the token care needed to keep the creature alive would introduce a variable it is not trying to measure.

Do not weaken an assertion. Do not delete a check. If one cannot be made to pass with an immortal config, stop and report it — that means death has broken something real.

- [ ] **Step 8: Add the Lua bindings**

In `simulator/src/lua/kf_lua_port.cpp`, beside the mess readers:

```cpp
/* Illness, readable from a script. neglect_seconds is exposed raw rather
 * than as a "distress level" enum because where the thresholds sit is
 * config: a script that wants three bands can compute them, and one that
 * wants five is not blocked by a choice made here. */
int lua_pet_sick(lua_State *L) {
    lua_pushboolean(L, kf_pet_session_state()->sick ? 1 : 0);
    return 1;
}

int lua_pet_dead(lua_State *L) {
    lua_pushboolean(L, kf_pet_session_state()->dead ? 1 : 0);
    return 1;
}

int lua_pet_neglect_seconds(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->neglect_seconds));
    return 1;
}
```

Register them in `kKfPetFuncs` as `"sick"`, `"dead"` and `"neglect_seconds"`.

- [ ] **Step 9: Prove the bindings against live state**

In `simulator/src/lua/kf_lua_pet_proof_script.h`, add — and update the header comment's script count and its per-script description, matching the style already there:

```cpp
inline constexpr const char *kKfLuaPetHealthProofScriptSource = R"lua(
local kSick = 1
local kDead = 2

function on_frame(dt_ms)
    local flags = 0
    if pet.sick() then flags = flags + kSick end
    if pet.dead() then flags = flags + kDead end
    kf.report(flags * 10000000 + pet.neglect_seconds())
end

kf.log("pet health proof script loaded")
)lua";

inline constexpr const char *kKfLuaPetHealthProofScriptChunkName =
    "=pet_health_proof_script";
```

Then add a stage to `run_lua_pet_check()`, immediately after the mess/clean stage, before `kf_pet_session_shutdown()`:

```cpp
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
```

If the creature turns out to *die* during those ten hours rather than merely falling ill, that is a legitimate outcome and the packing already covers it — but check the arithmetic rather than assuming, and shorten the stage if a live death makes the check less clear about what it is proving.

- [ ] **Step 10: Full verification**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

```bash
python3 tools/check_no_heap.py .
```

```bash
cd ports/esp32 && idf.py build
```

- [ ] **Step 11: Commit**

```bash
git add -A && git commit -m "Death: sustained critical neglect ends the creature, after a day of warning"
```

---

## What this deliberately does not do

- **No screen work.** Nothing draws illness, distress or death. The pet screen's layout pass is already pending on the 48×48 creature sprite, and guessing where a distress indicator goes now would only be undone. The state and bindings are here so that pass has something to draw.
- **No medicine action.** The spec rejected it: a fifth button that is only ever "press when red". Curing through care is the design; medicine is the recorded fallback if it proves too forgiving in play.
- **No death screen and no new-creature flow.** `kf_pet_init()` is the whole story for now. What the player actually sees when a creature dies is a design conversation, not an implementation detail.
- **No babysitter.** Still deferred, and death-with-warning is what gives it a job later: the sanctioned way to hand over responsibility rather than a convenience.

## Report back

Beyond the usual, three things:

1. **Whether the untouched-creatures-do-not-sicken rule still feels right once it is implemented** rather than merely reasoned about.
2. **Every existing check that needed an immortal config**, listed. That list is the honest measure of how much of the suite was quietly assuming a creature that cannot die.
3. **Any place the both-ends sampling produced a surprising result** — it is the least obvious thing in this plan and the most likely to be subtly wrong.
