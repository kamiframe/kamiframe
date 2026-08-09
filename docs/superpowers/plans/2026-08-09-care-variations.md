# Care Variations and Preferences Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each of the four care actions has three variations, and every creature likes one of them, tolerates one, and dislikes one — according to a fixed table the player learns by experiment.

**Architecture:** The preference table is a pure function of base trait and action, `constexpr` data in `pet.cpp`, queryable without a creature. Care actions take a variation index and restore an amount that depends on whether the creature liked it. The reaction is recorded on the state so the screen and scripts can show it — that reaction, not the bar, is what the player reads.

**Tech Stack:** C++17, no exceptions/RTTI, heap-free core. Tests are subcommands of the headless simulator binary.

## Global Constraints

- **Core stays heap-free.** `python3 tools/check_no_heap.py .` must print `core is heap-free`.
- **Integer maths only, millipercent (0..100000).** No floating point in `pet.cpp`.
- **Warnings are errors:** `-DKAMIFRAME_WARNINGS_AS_ERRORS=ON`. GCC and MSVC both build this.
- **The device build must keep building** (`ports/esp32`).
- **Save format** is packed byte by byte; a new field means bumping `kSaveVersion` and `KF_PET_SAVE_BYTES` together.
- **No names, anywhere in Core.** Traits, forms and now variations are opaque 0-based indices. What variation 2 of feeding actually *is* — a snack, a proper meal, a treat — is creative content that lives in the Lua cartridge layer. `kf/pet.h`'s header comment already draws this line twice; do not be the first to cross it.
- **Comments explain why, not what.** Match the surrounding file.

### Current signatures

```cpp
void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config);
void kf_pet_play(kf_pet_state *state);
void kf_pet_rest(kf_pet_state *state);
void kf_pet_clean(kf_pet_state *state);
```

All four gain a variation argument in Task 1, and the three that do not currently take a config gain one — this is the single coordinated pass the mess plan deferred them to, rather than four separate ripples through every call site.

---

## Design decisions made here

### Preferences live on the base trait, and the table is fixed

Not rolled per creature. The spec's section 10 settled this: knowledge has to transfer, so a player can learn that trait 3 likes the second kind of play, be *right*, and have that hold for every future creature with that trait. A per-creature roll makes discovery unfalsifiable — you can never tell a mislearned type from an unusual individual.

Six traits × four actions = 24 facts about the world. Small enough to hold in your head, and small enough that a tuning mistake in it is findable.

### The table is a pure function, not creature state

`kf_pet_reaction_to(base_trait, action, variation)` takes no creature and touches no state. The screen can use it to preview, a script can use it to explain, and a test can exercise all 72 combinations without constructing a single creature.

### What the developed trait does is still not decided, and this does not decide it

The spec's section 5 says repeated disliked care should push the developed traits, and then explicitly says the mechanism is unresolved and must not be implemented before it is decided. That still holds. This plan implements liked/neutral/disliked as a **restore magnitude and a reaction**, and nothing else. The developed-trait push is deliberately absent, not forgotten.

### All twelve variations are available immediately

Unlocking through play is section 8's deferred work. Everything is unlocked so the interactions can be tuned, which is what Chris asked for.

---

## Task 1: The preference table

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp`
- Modify: `simulator/src/headless/headless_main.cpp`
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `KF_PET_BASE_TRAIT_COUNT` (6)
- Produces: `kf_pet_care_action` enum, `KF_PET_CARE_ACTION_COUNT` (4), `KF_PET_CARE_VARIATION_COUNT` (3), `kf_pet_reaction` enum, `uint8_t kf_pet_reaction_to(uint8_t base_trait, kf_pet_care_action action, uint8_t variation)`

- [ ] **Step 1: Write the failing test**

Add to `simulator/src/headless/headless_main.cpp` after `run_pet_death_check()`:

```cpp
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
```

Register the subcommand in `main()` — **match the codebase's actual flag convention, which is `--verify-<name>`; check the neighbouring dispatches rather than trusting this plan's wording.** Add the ctest entry beside the others.

- [ ] **Step 2: Run it to make sure it fails**

```bash
cmake --build build -j8
```

Expected: compile failure — no `kf_pet_reaction_to`.

- [ ] **Step 3: Add the types**

In `hakoniwaos/include/kf/pet.h`, after the base-trait count:

```cpp
/* The four care actions, as an index. Order is arbitrary but fixed: it is
 * the column order of the preference table and the argument to
 * kf_pet_reaction_to(), so reordering it silently rewrites every creature's
 * preferences. */
typedef enum {
    KF_PET_CARE_FEED = 0,
    KF_PET_CARE_PLAY = 1,
    KF_PET_CARE_REST = 2,
    KF_PET_CARE_CLEAN = 3,
} kf_pet_care_action;

#define KF_PET_CARE_ACTION_COUNT 4u

/* How many ways there are to perform each action. Three, per the care-loop
 * spec's section 8: enough to prove the shape, where five is five times the
 * art and tuning before the loop is known to be fun.
 *
 * WHAT each variation IS -- a snack or a proper meal, a bath or a wipe --
 * is creative content and lives in the cartridge layer, exactly like the
 * trait and evolution names. Core only ever knows there are three. */
#define KF_PET_CARE_VARIATION_COUNT 3u

/* How a creature took it. The player reads this, not the bars: with six
 * traits, four actions and three variations there is no discovering
 * anything from a bar that moved slightly less than expected. */
typedef enum {
    KF_PET_REACTION_LIKED = 0,
    KF_PET_REACTION_NEUTRAL = 1,
    KF_PET_REACTION_DISLIKED = 2,
} kf_pet_reaction;

/* How a creature with `base_trait` reacts to `variation` of `action`.
 *
 * A pure function of the table, taking no creature: the screen can preview
 * it, a script can explain it, and a test can walk all seventy-two
 * combinations without constructing anything. Preferences live on the BASE
 * trait, which is rolled once and never changes, so this answer is stable
 * for a creature's whole life and transfers to every future creature that
 * shares the trait -- which is what makes learning it worth the player's
 * time. See the care-loop spec's section 5.
 *
 * Out-of-range input returns KF_PET_REACTION_NEUTRAL rather than reading
 * past the table: a corrupted save that survived the version check should
 * land somewhere boring and defined. */
uint8_t kf_pet_reaction_to(uint8_t base_trait, kf_pet_care_action action,
                            uint8_t variation);
```

- [ ] **Step 4: Add the table**

In `pet.cpp`, in the anonymous namespace:

```cpp
/* The preference table: for each base trait and action, which variation is
 * liked and which is disliked. The third is neutral by elimination, which
 * is why only two numbers are stored -- a full 6x4x3 table of reactions
 * could encode a creature with two favourites, and this cannot.
 *
 * Placeholder values, in the same spirit as the decay rates: the SHAPE is
 * the decision (every trait wants something different, and no two traits
 * want the same set of things), and the specific assignments are for
 * living with. Chris tunes this once there is a creature to tune it
 * against.
 *
 * Rows are base traits, columns are kf_pet_care_action in enum order. */
constexpr uint8_t kLikedVariation[KF_PET_BASE_TRAIT_COUNT]
                                 [KF_PET_CARE_ACTION_COUNT] = {
    {0u, 1u, 2u, 0u},
    {1u, 2u, 0u, 2u},
    {2u, 0u, 1u, 1u},
    {0u, 2u, 1u, 2u},
    {1u, 0u, 2u, 1u},
    {2u, 1u, 0u, 0u},
};

constexpr uint8_t kDislikedVariation[KF_PET_BASE_TRAIT_COUNT]
                                     [KF_PET_CARE_ACTION_COUNT] = {
    {1u, 2u, 0u, 1u},
    {2u, 0u, 1u, 0u},
    {0u, 1u, 2u, 2u},
    {2u, 1u, 0u, 0u},
    {0u, 2u, 1u, 2u},
    {1u, 0u, 2u, 1u},
};
```

**Verify by hand before building** that no cell has `kLikedVariation == kDislikedVariation`, and that no two rows are identical across all four columns. The test checks both, but a table you have not read is a table you do not know.

And the function:

```cpp
uint8_t kf_pet_reaction_to(uint8_t base_trait, kf_pet_care_action action,
                            uint8_t variation) {
    const unsigned a = static_cast<unsigned>(action);
    if (base_trait >= KF_PET_BASE_TRAIT_COUNT ||
        a >= KF_PET_CARE_ACTION_COUNT ||
        variation >= KF_PET_CARE_VARIATION_COUNT) {
        return KF_PET_REACTION_NEUTRAL;
    }
    if (variation == kLikedVariation[base_trait][a]) {
        return KF_PET_REACTION_LIKED;
    }
    if (variation == kDislikedVariation[base_trait][a]) {
        return KF_PET_REACTION_DISLIKED;
    }
    return KF_PET_REACTION_NEUTRAL;
}
```

- [ ] **Step 5: Run the test, then the suite**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Expected: the new check passes and nothing else moves — this task adds a pure function nothing calls yet.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "The preference table: what each trait likes, and what it will not thank you for"
```

---

## Task 2: Variations reach the care actions

**Files:**
- Modify: `hakoniwaos/include/kf/pet.h`
- Modify: `hakoniwaos/src/pet.cpp`
- Modify: `simulator/src/pet/kf_pet_session.cpp`, `kf_pet_session.h`
- Modify: `simulator/src/lua/kf_lua_port.cpp`
- Modify: `simulator/src/lvgl/kf_pet_screen.cpp`
- Modify: `simulator/src/headless/headless_main.cpp`

**Interfaces:**
- Produces: all four care actions take `(state, config, variation)`; `kf_pet_state::last_reaction` and `last_care_action`; config `care_boost_liked_mp`, `care_boost_neutral_mp`, `care_boost_disliked_mp`; save version 8

- [ ] **Step 1: Write the failing test**

Add a new check, `run_pet_care_variation_check()`:

```cpp
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
    kf_pet_clean(&pet, &config, 0u);
    check(pet.last_care_action == KF_PET_CARE_CLEAN, "cleaning records itself");
    check(pet.poop_count == 0u, "and still does its actual job");

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
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cmake --build build -j8
```

Expected: compile failure — the care actions take no variation.

- [ ] **Step 3: Change the signatures**

In `kf/pet.h`:

```cpp
/* Care actions. Each raises its need and clamps at KF_PET_MILLIPERCENT_MAX
 * -- feeding an already-full creature does nothing extra, it does not bank
 * overfeeding against future decay.
 *
 * `variation` is which of the KF_PET_CARE_VARIATION_COUNT ways of doing it
 * was chosen. How much it restores depends on how this creature's base
 * trait feels about that variation (kf_pet_reaction_to()), and the reaction
 * is recorded on the state for the screen to show. Out-of-range is treated
 * as neutral, not rejected: a cartridge passing a bad index should get a
 * dull creature, not a dead one. */
void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);
void kf_pet_play(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);
void kf_pet_rest(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation);
void kf_pet_clean(kf_pet_state *state, const kf_pet_config *config,
                   uint8_t variation);
```

New state fields, after `dead`:

```cpp
    /* How the creature took the last care action, and which action it was.
     * Saved, so a creature reloaded mid-sulk is still sulking. This is the
     * feedback channel the spec's section 6 puts first: the reaction leads
     * and the bars confirm, because a reaction is readable at a glance on a
     * two-inch screen and a three-thousand-millipercent difference is not. */
    uint8_t last_reaction;    /* kf_pet_reaction */
    uint8_t last_care_action; /* kf_pet_care_action */
```

New config fields:

```cpp
    /* What a care action restores, by how the creature took it. The gap
     * between liked and disliked is the discovery signal: too small and the
     * player cannot tell which is which without a spreadsheet, too large
     * and getting it wrong is punishing rather than informative. */
    kf_pet_millipercent care_boost_liked_mp;
    kf_pet_millipercent care_boost_neutral_mp;
    kf_pet_millipercent care_boost_disliked_mp;
```

Defaults, replacing the `kCareBoostMp` constant in `pet.cpp` (delete it — a tuning value that lives in two places will eventually disagree with itself):

```cpp
    c.care_boost_liked_mp = 35000u;
    c.care_boost_neutral_mp = 25000u;
    c.care_boost_disliked_mp = 10000u;
```

`kf_pet_init()` sets `last_reaction = KF_PET_REACTION_NEUTRAL` and `last_care_action = KF_PET_CARE_FEED`.

- [ ] **Step 4: Implement**

In `pet.cpp`, a shared helper — the four actions differ only in which need they touch, and four copies of the reaction logic would be four places for it to drift:

```cpp
/* Resolves the reaction, records it, and returns what this action is worth.
 * Shared by all four care actions: they differ in which need they raise,
 * not in how preference works. */
kf_pet_millipercent apply_care_reaction(kf_pet_state *state,
                                         const kf_pet_config *config,
                                         kf_pet_care_action action,
                                         uint8_t variation) {
    const uint8_t reaction =
        kf_pet_reaction_to(state->base_trait, action, variation);
    state->last_reaction = reaction;
    state->last_care_action = static_cast<uint8_t>(action);
    switch (reaction) {
    case KF_PET_REACTION_LIKED:
        return config->care_boost_liked_mp;
    case KF_PET_REACTION_DISLIKED:
        return config->care_boost_disliked_mp;
    default:
        return config->care_boost_neutral_mp;
    }
}
```

Each action becomes, e.g.:

```cpp
void kf_pet_feed(kf_pet_state *state, const kf_pet_config *config,
                  uint8_t variation) {
    if (state->dead) {
        return;
    }
    if (state->care_actions_taken < UINT32_MAX) {
        state->care_actions_taken++;
    }
    const kf_pet_millipercent boost =
        apply_care_reaction(state, config, KF_PET_CARE_FEED, variation);
    state->hunger_mp = clamp_add(state->hunger_mp, boost);

    /* ...the existing poop-timer shortening, unchanged... */
}
```

`kf_pet_clean()` restores no need, so it records the reaction and ignores the returned boost — cast it to `(void)` with a one-line comment saying cleaning has no bar to raise, so that the unused value is visibly deliberate rather than an oversight the compiler happened not to catch.

- [ ] **Step 5: Update every call site**

Session wrappers take a variation and pass it through. The Lua bindings take an optional variation argument (`luaL_optinteger(L, 1, 0)` — a script that does not care about variations keeps working unchanged, which matters because the demo creature script is one of them). The pet screen's three button callbacks pass `0`.

Find them all:

```bash
grep -rn "kf_pet_feed\|kf_pet_play\|kf_pet_rest\|kf_pet_clean\|session_feed\|session_play\|session_rest\|session_clean" --include=*.cpp --include=*.h .
```

- [ ] **Step 6: Bump the save format**

`kSaveVersion` to 8, `KF_PET_SAVE_BYTES` to 91 (89 + 2), the two new bytes packed and unpacked, both comments extended.

- [ ] **Step 7: Run everything**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

Existing checks that call a care action will need the new argument. **Passing `0` is the right update for a check that is not about variations** — but a check that asserts a specific restore amount is now asserting something trait-dependent, since variation 0 is liked by some traits and disliked by others. If one of those fails, the honest fix is to make it set `base_trait` explicitly and pick the variation it means, not to soften the assertion.

```bash
python3 tools/check_no_heap.py .
```

```bash
cd ports/esp32 && idf.py build
```

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "Care actions take a variation, and the creature has opinions about it"
```

---

## Task 3: Reaching the player

**Files:**
- Modify: `simulator/src/lua/kf_lua_port.cpp`
- Modify: `simulator/src/lua/kf_lua_pet_proof_script.h`
- Modify: `simulator/src/headless/headless_main.cpp`

**Interfaces:**
- Produces: Lua `pet.last_reaction()`, `pet.last_care_action()`, `pet.reaction_to(trait, action, variation)`

- [ ] **Step 1: Add the bindings**

```cpp
/* The reaction to the last care action, and what it was a reaction to.
 * Integers rather than strings, unlike pet.stage(): a script showing a
 * reaction is picking a sprite or an animation, not printing a word, and
 * the cartridge layer owns what "liked" looks like for its creature. */
int lua_pet_last_reaction(lua_State *L) {
    lua_pushinteger(
        L, static_cast<lua_Integer>(kf_pet_session_state()->last_reaction));
    return 1;
}

int lua_pet_last_care_action(lua_State *L) {
    lua_pushinteger(L, static_cast<lua_Integer>(
                            kf_pet_session_state()->last_care_action));
    return 1;
}

/* pet.reaction_to(trait, action, variation) -- the table itself, queryable
 * without performing the action. This is what lets a cartridge build a
 * "what does this one like?" screen, or a test creature explain itself,
 * without the player having to try everything on a live creature first. */
int lua_pet_reaction_to(lua_State *L) {
    const lua_Integer trait = luaL_checkinteger(L, 1);
    const lua_Integer action = luaL_checkinteger(L, 2);
    const lua_Integer variation = luaL_checkinteger(L, 3);
    lua_pushinteger(L, static_cast<lua_Integer>(kf_pet_reaction_to(
                            static_cast<uint8_t>(trait),
                            static_cast<kf_pet_care_action>(action),
                            static_cast<uint8_t>(variation))));
    return 1;
}
```

`kf_pet_reaction_to()` already clamps out-of-range input to neutral, so a script passing rubbish gets a dull answer rather than an error — which is the right shape for a cartridge API, but note that `luaL_checkinteger` will still reject a non-number, which is a script *type* error rather than a value error and should surface.

- [ ] **Step 2: Prove them against live state**

Add a proof script and a stage to `run_lua_pet_check()`, in the established shape: feed the live session a variation whose reaction is known from C++, then compare `pet.last_reaction()` against the same value read directly from `kf_pet_session_state()`. Assert the reaction is not neutral before comparing, so the check cannot pass on two zeroes — the same trap the mess and health stages guard against.

The session's creature has a randomly rolled `base_trait`, so the test must ask `kf_pet_reaction_to()` which variation that trait likes rather than assuming one.

- [ ] **Step 3: Full verification and commit**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```

```bash
python3 tools/check_no_heap.py .
```

```bash
cd ports/esp32 && idf.py build
```

```bash
git add -A && git commit -m "Variations and reactions reach the cartridge layer"
```

---

## What this deliberately does not do

- **No developed-trait push.** The spec says disliked care should shape who the creature becomes and then says the mechanism is undecided. It stays undecided.
- **No unlock progression.** All twelve available, as Chris asked, so the interactions can be tuned.
- **No screen work.** Nothing draws a reaction yet. The pet screen's three buttons pass variation 0 and look exactly as they do now. Choosing a variation is a UI design question that belongs with the layout pass, and inventing a picker now would only be undone.
- **No names for anything.** Core knows there are three variations. What they are is content.

## Report back

Beyond the usual: **whether the placeholder preference table produced a creature that felt like it had opinions**, and whether the gap between liked and disliked (35000 vs 10000 millipercent) reads as informative or as punishing when you look at what it does to a bar. That number is the discovery signal, and it is the one most likely to be wrong on the first attempt.
