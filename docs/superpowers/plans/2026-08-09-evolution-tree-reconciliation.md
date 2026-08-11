# Evolution Tree Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Status: COMPLETE

`evolution_tree_shape_check` is registered in `simulator/CMakeLists.txt` and
passing. `KF_PET_ADULT_BRANCH_MAX` now exists in `hakoniwaos/include/kf/pet.h`
— the "does not exist" note below Step 2 describes the expected compile
failure *before* Step 3 creates it, part of that step's own TDD sequence, not
the state of the tree today.

**Goal:** Bring the simulation's evolution tree in line with the character bible — four verb families instead of three teen forms, an uneven number of adults per family instead of a uniform two, and a separate path to Hokorimaru for a creature that is never interacted with.

**Architecture:** The stage machine does not change: five stages, one transition at a time, driven by `kf_pet_advance()`. What changes is the branch *shape* — two compile-time constants become a constant plus a per-family table, `select_branch()` learns to clamp against a per-family count, and a new "never interacted" condition is checked at the child-to-teen transition. `teen_form` and `adult_branch` are saved, so the save format version must rise.

**Tech Stack:** C++17, no exceptions, no RTTI (ADR 0001). Core is heap-free (ADR 0008) — this adds a small `static constexpr` table, no allocation.

## Global Constraints

- **HakoniwaOS is always one token.** Never bare "Hakoniwa".
- **Never invent creature names, lore, or roster entries.** Everything comes from `14-character-bible-v1.md`. All names in it are UNVERIFIED placeholders — the code should carry indices, not names, exactly as it does today (see `kf/pet.h`'s existing "PERSONALITY, LIKE EVOLUTION, IS NUMBERS ONLY HERE" comment). Do not add a name table.
- **Core stays heap-free.** `python3 tools/check_no_heap.py .` must keep reporting `core is heap-free`.
- **Zero warnings** on both builds.
- **Every ctest case must pass at the end.** There are 15 as of the demand-curve plan landing (14 plus `pet_demand_curve_check`); confirm the actual count before you start rather than trusting this number.
- A save written before this change **must be refused, not misread.** The version bump is what does that.

---

## The shape being implemented

From the character bible, sections 6, 7 and 8, as resolved in
`docs/superpowers/specs/2026-08-09-core-care-loop-design.md`:

| Family index | Verb | Teen form | Adults |
|---|---|---|---|
| 0 | Cut | Hamaru | 2 |
| 1 | Hold | Nigimaru | 3 |
| 2 | Mark | Tenmaru | 3 |
| 3 | Go | Ayumaru | 1 |

Plus **Hokorimaru**, which has no family and is reached only by never
interacting with the creature at all.

Names are listed here for the reader's orientation only. **They do not appear
in the code.**

---

### Task 1: Four families, uneven adult counts

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp` (`select_branch()` and its call sites)
- Modify: `simulator/src/lvgl/kf_pet_screen.cpp` (the `kTeenColor` and `kAdultColor` tables are sized by these constants)
- Test: `simulator/src/headless/headless_main.cpp`

**Interfaces:**
- Consumes: `KF_PET_TEEN_FORM_COUNT`, `KF_PET_ADULT_BRANCH_COUNT` (both being replaced); `select_branch(uint64_t, uint64_t, uint32_t)`.
- Produces: `KF_PET_TEEN_FORM_COUNT` == 4u; `KF_PET_ADULT_BRANCH_MAX` == 3u; `kf_pet_adults_in_family(uint8_t teen_form) -> uint8_t`.

- [ ] **Step 1: Write the failing test**

Add to `simulator/src/headless/headless_main.cpp`, before `run_pet_screen_check`:

```cpp
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
    return ok ? 0 : 1;
}
```

Wire it in exactly as the neighbouring checks are: a `bool verify_tree_shape`
flag, a `--verify-tree-shape` argument branch, a usage line, a dispatch, and
an `add_test(NAME evolution_tree_shape_check COMMAND kamiframe-headless
--verify-tree-shape)` in `simulator/CMakeLists.txt`.

- [ ] **Step 2: Run it and watch it fail**

```bash
cmake --build build-desktop --target kamiframe-headless --parallel
```

Expected: FAILS to compile — `kf_pet_adults_in_family` is undeclared, and
`KF_PET_ADULT_BRANCH_MAX` does not exist.

- [ ] **Step 3: Replace the constants**

In `hakoniwaos/include/kf/pet.h`, find the block defining
`KF_PET_TEEN_FORM_COUNT` and `KF_PET_ADULT_BRANCH_COUNT` and replace both
with:

```c
/* Four verb families -- Cut, Hold, Mark, Go -- per the character bible's
 * section 6. Indices only: the bible's names are unverified placeholders and
 * deliberately do not appear in Core, exactly like base_trait above. */
#define KF_PET_TEEN_FORM_COUNT 4u

/* The largest number of adults any one family has. Sizes arrays that must
 * hold a row per family; the ACTUAL count per family is uneven and comes
 * from kf_pet_adults_in_family(). */
#define KF_PET_ADULT_BRANCH_MAX 3u

/* How many adult forms a given verb family leads to.
 *
 * Uneven on purpose, and expected to change: the bible's confirmed roster is
 * Cut 2, Hold 3, Mark 3, Go 1, and its own section 11 says Go and Cut still
 * need creatures to balance at three each. A per-family lookup rather than
 * one constant means filling those gaps is a one-line data edit instead of a
 * change to the tree's shape.
 *
 * Out-of-range input returns 1 rather than reading past the table -- a
 * corrupted save that survived the version check should land on a valid
 * creature, not undefined behaviour. */
uint8_t kf_pet_adults_in_family(uint8_t teen_form);
```

- [ ] **Step 4: Implement the lookup**

In `hakoniwaos/src/pet.cpp`, at file scope inside the anonymous namespace:

```cpp
/* Cut, Hold, Mark, Go -- character bible section 6, in that index order. */
constexpr uint8_t kAdultsInFamily[KF_PET_TEEN_FORM_COUNT] = {2u, 3u, 3u, 1u};
```

and outside the anonymous namespace, with the other public functions:

```cpp
uint8_t kf_pet_adults_in_family(uint8_t teen_form) {
    if (teen_form >= KF_PET_TEEN_FORM_COUNT) {
        return 1u;
    }
    return kAdultsInFamily[teen_form];
}
```

- [ ] **Step 5: Use it at the branch point**

Find where `select_branch()` is called with `KF_PET_ADULT_BRANCH_COUNT` for
the teen-to-adult transition. Replace that argument with
`kf_pet_adults_in_family(state->teen_form)`.

Find where it is called for the child-to-teen transition with
`KF_PET_TEEN_FORM_COUNT` — that call is unchanged, since every child can
reach any of the four families.

- [ ] **Step 6: Fix the screen's colour tables**

`simulator/src/lvgl/kf_pet_screen.cpp` sizes `kTeenColor` by
`KF_PET_TEEN_FORM_COUNT` and `kAdultColor` by
`[KF_PET_TEEN_FORM_COUNT][KF_PET_ADULT_BRANCH_COUNT]`. Both are now the wrong
size and will not compile.

Add a fourth row to `kTeenColor` and resize `kAdultColor` to
`[KF_PET_TEEN_FORM_COUNT][KF_PET_ADULT_BRANCH_MAX]`, filling the unused slots
in short families with the family's own teen colour. Add a comment saying
these are placeholder colours standing in for art, that rows are ragged
because families are, and that unused slots are unreachable rather than
meaningful.

- [ ] **Step 7: Run the test**

```bash
cmake --build build-desktop --target kamiframe-headless --parallel && ./build-desktop/kamiframe-headless --verify-tree-shape
```

Expected: PASS, logging `tree: 4 families, 9 adults, max 3 per family`.

- [ ] **Step 8: Run the whole suite**

```bash
ctest --test-dir build-desktop --output-on-failure
```

`pet_stage_evolution_check` will very likely FAIL — it asserts against a
3 x 2 tree. Read the failure, work out what the new shape predicts, and update
the expectation. Do NOT loosen the assertion.

- [ ] **Step 9: Commit**

```bash
git add hakoniwaos/include/kf/pet.h hakoniwaos/src/pet.cpp \
        simulator/src/lvgl/kf_pet_screen.cpp \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "Evolution tree: four verb families with uneven adult counts, per the bible"
```

---

### Task 2: The never-interacted path to Hokorimaru

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h` (state field, save version, the constant)
- Modify: `hakoniwaos/src/pet.cpp` (`kf_pet_init()`, the care functions, the child-to-teen transition)
- Test: `simulator/src/headless/headless_main.cpp`

**Interfaces:**
- Consumes: Task 1's `KF_PET_TEEN_FORM_COUNT`; the existing `kf_pet_feed()`, `kf_pet_play()`, `kf_pet_rest()`; the existing save-version constant.
- Produces: `KF_PET_TEEN_FORM_DUST` == 4u; `kf_pet_state::care_actions_taken`.

**A design decision this task makes, flagged for review:** the bible says
Hokorimaru "has no verb and no juvenile stage." Implementing a transition that
*skips* a stage would be a special case running through the whole stage
machine, for one creature. Instead this adds a **fifth teen form**, reachable
only by the never-interacted condition, whose single adult is Hokorimaru — so
the creature still passes through teen, as proto-dust. That is a small
deviation from the bible's wording and it is the cheapest faithful option; if
Chris wants the stage genuinely skipped, this is the task to revisit.

- [ ] **Step 1: Write the failing test**

```cpp
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

    /* Never touched: straight through to the dust form. */
    kf_pet_state neglected{};
    kf_pet_init(&neglected);
    check(neglected.care_actions_taken == 0u, "a fresh pet has taken no care");
    advance_to_teen_for_test(&neglected, &config);
    check(neglected.teen_form == KF_PET_TEEN_FORM_DUST,
          "a never-touched creature becomes the dust form");

    /* Touched even once: an ordinary family. */
    kf_pet_state touched{};
    kf_pet_init(&touched);
    kf_pet_feed(&touched);
    check(touched.care_actions_taken == 1u, "feeding counts as care");
    advance_to_teen_for_test(&touched, &config);
    check(touched.teen_form != KF_PET_TEEN_FORM_DUST,
          "one care action is enough to avoid the dust form");
    check(touched.teen_form < KF_PET_TEEN_FORM_COUNT,
          "a cared-for creature lands in one of the four families");

    return ok ? 0 : 1;
}
```

`advance_to_teen_for_test()` is a helper you write beside it: call
`kf_pet_advance()` with enough elapsed time to carry the pet from egg through
baby and child into teen, using `config`'s own stage durations rather than
hardcoded seconds.

- [ ] **Step 2: Run it and watch it fail**

Expected: FAILS to compile — `care_actions_taken` and
`KF_PET_TEEN_FORM_DUST` do not exist.

- [ ] **Step 3: Add the constant and the counter**

In `hakoniwaos/include/kf/pet.h`, after `KF_PET_TEEN_FORM_COUNT`:

```c
/* The dust form. NOT one of the four verb families -- deliberately equal to
 * KF_PET_TEEN_FORM_COUNT so it sits just past them, and so any loop over the
 * families skips it. Reached only by the never-interacted condition; see
 * kf_pet_state::care_actions_taken. */
#define KF_PET_TEEN_FORM_DUST KF_PET_TEEN_FORM_COUNT
```

In `kf_pet_state`, beside the personality fields:

```c
    /* How many care actions this creature has EVER received. Not a rate, not
     * decayed, never reset: the only question it answers is "has anyone ever
     * touched this at all", which is what separates a neglected creature
     * (cared for badly) from an abandoned one (never cared for). Saturates
     * rather than wrapping -- the difference between 0 and 1 is the only one
     * that matters. */
    uint32_t care_actions_taken;
```

- [ ] **Step 4: Count care actions**

In `hakoniwaos/src/pet.cpp`, add to `kf_pet_feed()`, `kf_pet_play()` and
`kf_pet_rest()`, at the top of each:

```cpp
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
```

And in `kf_pet_init()`, beside the other field initialisations:

```cpp
    state->care_actions_taken = 0u;
```

- [ ] **Step 5: Branch on it**

At the child-to-teen transition, before the normal `select_branch()` call for
`teen_form`:

```cpp
        /* Never touched -> dust, regardless of what the care average says.
         * Checked BEFORE the ordinary branch selection because it is not a
         * quality judgement: an untouched creature has no care history to
         * grade, and grading it anyway would land it in whichever family
         * zero care happens to map to. */
        if (state->care_actions_taken == 0u) {
            state->teen_form = KF_PET_TEEN_FORM_DUST;
        } else {
            /* the existing select_branch() call, unchanged */
        }
```

At the teen-to-adult transition, the dust form has exactly one adult, and
`kf_pet_adults_in_family()` already returns 1 for out-of-range input — which
`KF_PET_TEEN_FORM_DUST` is, by construction. Verify that path lands on
`adult_branch == 0` and add a comment saying the out-of-range return is being
relied on deliberately here, not by accident.

- [ ] **Step 6: Bump the save version**

Find the save-format version constant in `kf/pet.h` (the header comment
mentions a save from an earlier version being refused) and increment it. Add
a line to its comment: `care_actions_taken` was added and the teen-form range
changed, so an older save's `teen_form` would be misread.

- [ ] **Step 7: Run the test, then the suite**

```bash
cmake --build build-desktop --target kamiframe-headless --parallel && ./build-desktop/kamiframe-headless --verify-hokorimaru
ctest --test-dir build-desktop --output-on-failure
```

Both must pass. A save-related test may fail on the version bump; if so, that
is the bump working, and the fixture needs regenerating rather than the check
weakening.

- [ ] **Step 8: Verify the device build and the heap rule**

```bash
python3 tools/check_no_heap.py .
cd ports/esp32 && source ~/esp/esp-idf/export.sh && idf.py build
```

- [ ] **Step 9: Commit**

```bash
git add hakoniwaos/include/kf/pet.h hakoniwaos/src/pet.cpp \
        simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "Hokorimaru: the creature you get by never touching it at all"
```

---

## What this plan deliberately does NOT do

- **Name anything.** Core carries indices; the bible's names are unverified
  placeholders and belong in the art manifest, not here.
- **Grudge forms.** The bible treats them as art variants of existing
  creatures, not tree entries. Nothing in the tree changes for them.
- **Fill the roster gaps.** Go and Cut are short creatures per the bible's
  section 11. The per-family table is what makes adding them cheap; choosing
  them is Chris's.
- **Anything from the care-loop spec** — variations, mess, sickness, sleep.
