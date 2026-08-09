# The Creature On Screen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The pet screen stops being an LVGL circle and becomes a real creature — a 48x48 sprite that walks around under its own direction, changes pose according to what the simulation already knows, and has its mess drawn beside it.

**Architecture:** The pet screen takes ownership of its pixels from LVGL (Chris's decision, 2026-08-09; the split ADR 0013 always intended). LVGL keeps menus and settings. Presentation state — where the creature is, which way it faces, which pose it shows — lives in `hakoniwaos/` as a new `kf_creature` module, *not* in `simulator/`, because the ESP32 build must run the identical code. Core's `kf_pet_state` is never modified by any of this: the creature module only ever reads it. Every behaviour decision goes through `kf/rng.h`, so a seed reproduces a run exactly.

**Tech Stack:** C++17, CMake, the existing `kf_blit`/`kf_fb_mark_dirty` framebuffer path, `kf/font.h` bitmap text (ADR 0010), CTest via `kamiframe-headless --verify-*` check modes.

## Global Constraints

- **240x320 RGB565.** No alpha channel anywhere — transparency is colour-key only, magenta `KF_RGB(255,0,255)` by the convention in `tools/kf_ingest_sprites.py:53`.
- **Creature sprites are 48x48.** Fixed by `tools/character_manifest.toml`'s `default_size`. Do not introduce a second size.
- **Maximum 8 dirty rectangles per frame** (`KF_MAX_DIRTY_RECTS`, `hakoniwaos/include/kf/framebuffer.h:44`). Past 8 the system collapses to one screen-sized bounding box and re-transfers ~31ms of pixels against a 33.3ms budget. This is the binding constraint of this entire plan — every task must state its rect cost.
- **Anything drawn outside a `kf_fb_mark_dirty()` region is never sent to the display.** `kf_blit`/`kf_fill_rect` mark dirty themselves; raw framebuffer writes do not.
- **`hakoniwaos/` must stay heap-free.** `python3 tools/check_no_heap.py` runs in `dev.sh test` and will fail the build. No `malloc`/`new` in any new Core file.
- **Core simulation is never mutated by presentation.** `kf_creature_*` takes `const kf_pet_state *`. If a task seems to need a writable pet, the design is wrong.
- **Sleep is specified but not yet in Core.** See `docs/superpowers/specs/2026-08-09-core-care-loop-design.md` § "Sleep, settled". This plan wires the *sleeping pose* but nothing can select it yet; that arrives with the separate sleep plan. Do not invent a sleep field to make it selectable.
- **Every character name is an unverified placeholder.** Do not treat `chokimaru` etc. as shippable.

---

### Task 1: Pose selection

Turn what the simulation knows into which sprite to show. Pure function, no HAL, no framebuffer, no allocation — the easiest thing in this plan to get right and the thing everything else depends on.

**Files:**
- Create: `hakoniwaos/include/kf/creature.h`
- Create: `hakoniwaos/src/creature.cpp`
- Modify: `hakoniwaos/sources.cmake` (add `src/creature.cpp` to the source list)
- Test: `simulator/src/headless/headless_main.cpp` (new `run_creature_pose_check()`)
- Modify: `simulator/CMakeLists.txt` (register `creature_pose_check`)

**Interfaces:**
- Consumes: `kf_pet_state` from `kf/pet.h` — reads `sick`, `dead`, `last_reaction`, `last_care_action`, `care_actions_taken` only.
- Produces: `kf_creature_pose` enum and `kf_creature_pose_for(const kf_pet_state *, uint32_t reaction_hold_ms)`. Tasks 2 and 4 both depend on these exact names.

**Design note the implementer must not get wrong:** `last_reaction` is *sticky* — it holds the last reaction forever, so selecting a pose from it directly would leave the creature permanently grinning after one liked feed. The reaction poses are therefore time-windowed by the caller, which passes how many milliseconds remain on the reaction hold. Zero means the window has expired.

- [ ] **Step 1: Write the failing test**

Add to `simulator/src/headless/headless_main.cpp`, near the other `run_*_check()` functions:

```cpp
static int run_creature_pose_check(void) {
    kf_pet_config cfg = kf_pet_default_config();
    kf_pet_state pet;
    kf_pet_init(&pet, &cfg);

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

    int failures = 0;
    for (const Case &c : cases) {
        pet.sick = c.sick;
        pet.dead = c.dead;
        pet.last_reaction = c.reaction;
        const kf_creature_pose got = kf_creature_pose_for(&pet, c.hold_ms);
        if (got != c.expect) {
            KF_LOGE("creature-pose", "%s: expected %d, got %d", c.name,
                    (int)c.expect, (int)got);
            ++failures;
        }
    }

    if (failures != 0) {
        KF_LOGE("creature-pose", "%d case(s) failed", failures);
        return 1;
    }
    KF_LOGI("creature-pose", "all %zu cases passed",
            sizeof(cases) / sizeof(cases[0]));
    return 0;
}
```

Wire the flag in, alongside the existing `--verify-*` handlers around `headless_main.cpp:2807`:

```cpp
} else if (std::strcmp(argv[i], "--verify-creature-pose") == 0) {
    return run_creature_pose_check();
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL to compile — `'kf_creature_pose_for' was not declared in this scope`, and `kf/creature.h` not found. That is the correct failure; do not proceed until you see it.

- [ ] **Step 3: Write the header**

Create `hakoniwaos/include/kf/creature.h`:

```c
/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The creature as a thing you can see, as opposed to kf/pet.h's creature as a
 * set of numbers. Nothing here ever changes the simulation; every function
 * takes the pet as const and reads it.
 *
 * This lives in hakoniwaos/ rather than simulator/ because the device draws
 * the same creature the desktop does, from the same code. A presentation
 * layer that existed only on the desktop would be the emulator this project
 * does not have.
 */

#ifndef KF_CREATURE_H
#define KF_CREATURE_H

#include "kf/pet.h"
#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which sprite to show. These are the five states the character manifest
 * draws for every creature, plus dead, which has no art yet -- see
 * kf_creature_sprite_name() in Task 2 for what happens meanwhile. */
typedef enum {
    KF_CREATURE_POSE_NEUTRAL = 0,
    KF_CREATURE_POSE_HAPPY,
    KF_CREATURE_POSE_OBJECTING,
    KF_CREATURE_POSE_SICK,
    KF_CREATURE_POSE_SLEEPING,
    KF_CREATURE_POSE_DEAD,
    KF_CREATURE_POSE_COUNT
} kf_creature_pose;

/* Pick the pose for the pet as it stands right now.
 *
 * reaction_hold_ms is how long the most recent care reaction should still be
 * showing on the creature's body, counted down by the caller. It exists
 * because kf_pet_state::last_reaction is sticky -- it keeps the last reaction
 * forever -- so reading it directly would leave a creature grinning for the
 * rest of its life after one liked feed. Zero means the reaction has finished
 * being expressed.
 *
 * Precedence, strongest first: dead, sick, then the held reaction, then
 * neutral. Sleeping is never returned yet: nothing in Core can say the
 * creature is asleep (see the care-loop spec's "Sleep, settled" addendum);
 * the pose exists so the sprite table and the art are ready when it lands. */
kf_creature_pose kf_creature_pose_for(const kf_pet_state *pet,
                                      uint32_t reaction_hold_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_CREATURE_H */
```

- [ ] **Step 4: Write the minimal implementation**

Create `hakoniwaos/src/creature.cpp`:

```cpp
/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf/creature.h"

kf_creature_pose kf_creature_pose_for(const kf_pet_state *pet,
                                      uint32_t reaction_hold_ms) {
    if (pet == nullptr) {
        return KF_CREATURE_POSE_NEUTRAL;
    }
    if (pet->dead) {
        return KF_CREATURE_POSE_DEAD;
    }
    if (pet->sick) {
        return KF_CREATURE_POSE_SICK;
    }
    if (reaction_hold_ms > 0u) {
        if (pet->last_reaction == KF_PET_REACTION_LIKED) {
            return KF_CREATURE_POSE_HAPPY;
        }
        if (pet->last_reaction == KF_PET_REACTION_DISLIKED) {
            return KF_CREATURE_POSE_OBJECTING;
        }
    }
    return KF_CREATURE_POSE_NEUTRAL;
}
```

Add `src/creature.cpp` to the source list in `hakoniwaos/sources.cmake`, following the existing entries exactly.

- [ ] **Step 5: Register the test**

In `simulator/CMakeLists.txt`, after the `pet_screen_check` block near line 400, matching its style:

```cmake
    add_test(NAME creature_pose_check
             COMMAND kamiframe-headless --verify-creature-pose)
```

- [ ] **Step 6: Run the test and make sure it passes**

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_pose_check --output-on-failure`
Expected: PASS, with `all 6 cases passed` in the output.

- [ ] **Step 7: Confirm Core stayed heap-free**

Run: `python3 tools/check_no_heap.py`
Expected: no findings. `creature.cpp` allocates nothing.

- [ ] **Step 8: Commit**

```bash
git add hakoniwaos/include/kf/creature.h hakoniwaos/src/creature.cpp hakoniwaos/sources.cmake simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "The creature has a pose, chosen from what the simulation knows"
```

---

### Task 2: Sprite names from stage and pose

The manifest's filenames are the contract between the art and the code (`<creature>_<pose>_<frame>`). This turns `(stage, pose)` into the name `kf_assets_get()` will be handed, so nothing downstream builds strings by hand.

**Files:**
- Modify: `hakoniwaos/include/kf/creature.h`
- Modify: `hakoniwaos/src/creature.cpp`
- Test: `simulator/src/headless/headless_main.cpp` (extend `run_creature_pose_check()`)

**Interfaces:**
- Consumes: `kf_creature_pose` from Task 1; `kf_pet_stage` from `kf/pet.h`.
- Produces: `kf_creature_sprite_name(kf_pet_stage, kf_creature_pose, char *out, size_t out_len)`. Task 4 calls this to look up the sprite.

**Design note:** the manifest gives the egg exactly one state, `idle` (`character_manifest.toml:93`), because the egg has no decay to react to. Every pose therefore collapses to `egg_idle_01` at that stage. Getting this wrong means the egg silently has no sprite and draws nothing.

- [ ] **Step 1: Write the failing test**

Append inside `run_creature_pose_check()`, before the `failures` check:

```cpp
    struct NameCase {
        kf_pet_stage stage;
        kf_creature_pose pose;
        const char *expect;
    };
    const NameCase names[] = {
        {KF_PET_STAGE_EGG, KF_CREATURE_POSE_NEUTRAL, "egg_idle_01"},
        {KF_PET_STAGE_EGG, KF_CREATURE_POSE_SICK, "egg_idle_01"},
        {KF_PET_STAGE_BABY, KF_CREATURE_POSE_NEUTRAL, "baby_neutral_01"},
        {KF_PET_STAGE_BABY, KF_CREATURE_POSE_SLEEPING, "baby_sleeping_01"},
        {KF_PET_STAGE_CHILD, KF_CREATURE_POSE_HAPPY, "child_happy_01"},
    };
    for (const NameCase &c : names) {
        char buf[32] = {0};
        kf_creature_sprite_name(c.stage, c.pose, buf, sizeof(buf));
        if (std::strcmp(buf, c.expect) != 0) {
            KF_LOGE("creature-pose", "name: expected '%s', got '%s'", c.expect,
                    buf);
            ++failures;
        }
    }
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL — `'kf_creature_sprite_name' was not declared in this scope`.

- [ ] **Step 3: Declare it**

Add to `kf/creature.h` before the closing `extern "C"`:

```c
/* Write the asset-pack name for this stage and pose into out, always
 * NUL-terminated. Names follow tools/character_manifest.toml's convention,
 * <entity>_<state>_<frame>, because that manifest is what produced the art
 * and its filenames are the only contract between the two.
 *
 * The egg has exactly one state ("idle") because it has nothing to react to,
 * so every pose collapses to egg_idle_01 there.
 *
 * KF_CREATURE_POSE_DEAD has no art in the manifest yet -- the death scene is
 * unbuilt (care-loop spec section 7). It falls back to the sick sprite, which
 * is wrong-looking but visible, rather than to nothing at all, which would
 * look like a rendering bug. */
void kf_creature_sprite_name(kf_pet_stage stage, kf_creature_pose pose,
                             char *out, size_t out_len);
```

- [ ] **Step 4: Implement it**

Add to `hakoniwaos/src/creature.cpp` (add `#include <stdio.h>` and `#include <string.h>` at the top):

```cpp
namespace {

const char *stage_name(kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG: return "egg";
    case KF_PET_STAGE_BABY: return "baby";
    case KF_PET_STAGE_CHILD: return "child";
    case KF_PET_STAGE_TEEN: return "teen";
    case KF_PET_STAGE_ADULT: return "adult";
    default: return "baby";
    }
}

const char *pose_name(kf_creature_pose pose) {
    switch (pose) {
    case KF_CREATURE_POSE_HAPPY: return "happy";
    case KF_CREATURE_POSE_OBJECTING: return "objecting";
    case KF_CREATURE_POSE_SICK: return "sick";
    case KF_CREATURE_POSE_SLEEPING: return "sleeping";
    case KF_CREATURE_POSE_DEAD: return "sick"; /* no death art yet */
    case KF_CREATURE_POSE_NEUTRAL:
    default: return "neutral";
    }
}

} // namespace

void kf_creature_sprite_name(kf_pet_stage stage, kf_creature_pose pose,
                             char *out, size_t out_len) {
    if (out == nullptr || out_len == 0u) {
        return;
    }
    if (stage == KF_PET_STAGE_EGG) {
        snprintf(out, out_len, "egg_idle_01");
        return;
    }
    snprintf(out, out_len, "%s_%s_01", stage_name(stage), pose_name(pose));
}
```

- [ ] **Step 5: Run the test and make sure it passes**

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_pose_check --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add hakoniwaos/include/kf/creature.h hakoniwaos/src/creature.cpp simulator/src/headless/headless_main.cpp
git commit -m "A stage and a pose name the sprite the pack must hold"
```

---

### Task 3: A mind of its own

The wander. The creature picks somewhere to be, walks there, stands about for a while, then picks again. Deterministic given a seed, so it is testable and so a bug report reproduces.

**Files:**
- Modify: `hakoniwaos/include/kf/creature.h`
- Modify: `hakoniwaos/src/creature.cpp`
- Test: `simulator/src/headless/headless_main.cpp` (new `run_creature_wander_check()`)
- Modify: `simulator/CMakeLists.txt`

**Interfaces:**
- Consumes: `kf/rng.h`'s `kf_rng_below()`; `kf_rect` from `kf/types.h`.
- Produces: `kf_creature` struct, `kf_creature_init()`, `kf_creature_update()`, `kf_creature_bounds()`. Task 4 owns an instance and calls `update` once per frame.

**Design note:** positions are in 1/16th pixels, the same trick `hakoniwaos/src/demo.cpp:25` uses, so movement is smooth without floating point (the ESP32-S3 FPU is single-precision and Lua is built for it, but Core stays integer). `KF_CREATURE_SUB` is that scale factor.

- [ ] **Step 1: Write the failing test**

```cpp
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
    if (a.x != b.x || a.y != b.y) {
        KF_LOGE("creature-wander", "same seed diverged: (%d,%d) vs (%d,%d)",
                a.x, a.y, b.x, b.y);
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
                KF_LOGE("creature-wander",
                        "seed %u escaped the field at step %d: (%d,%d,%d,%d)",
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
        KF_LOGE("creature-wander", "never moved in 2000 steps");
        ++failures;
    }

    if (failures != 0) { return 1; }
    KF_LOGI("creature-wander", "determinism, bounds and movement all hold");
    return 0;
}
```

Wire in `--verify-creature-wander` beside the other flags.

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL — `'kf_creature' does not name a type`.

- [ ] **Step 3: Declare the type and its three functions**

Add to `kf/creature.h`:

```c
/* Sub-pixel scale for creature positions. Movement is integer maths at 1/16th
 * of a pixel, the same approach hakoniwaos/src/demo.cpp uses, so a slow walk
 * is smooth without floating point in Core. */
#define KF_CREATURE_SUB 16

/* Presentation state for one creature. Not saved -- where the creature
 * happens to be standing is not worth persisting, and a fresh position on
 * load is indistinguishable from a remembered one. */
typedef struct {
    int32_t x;              /* 1/16th pixels, top-left of the sprite */
    int32_t y;
    int32_t target_x;       /* where it has decided to walk to */
    int32_t target_y;
    int16_t facing;         /* -1 facing left, +1 facing right */
    uint32_t dwell_ms;      /* how long it still intends to stand still */
    uint32_t reaction_hold_ms;
    uint32_t seen_care_actions; /* to notice a new care action happening */
} kf_creature;

/* Place the creature in the middle of the field and give it a first idea. */
void kf_creature_init(kf_creature *c, kf_rect field);

/* Advance one frame's worth of walking about. Pure apart from kf/rng.h, so a
 * seed reproduces a walk exactly. */
void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms);

/* Where the sprite currently sits, in whole pixels -- what to blit and what
 * to mark dirty. */
kf_rect kf_creature_bounds(const kf_creature *c);
```

- [ ] **Step 4: Implement**

```cpp
namespace {

/* Whole pixels per second, slow enough to read as an animal pottering about
 * rather than a cursor being dragged. */
constexpr int32_t kSpeedPxPerSec = 18;
constexpr int32_t kSpriteSize = 48;
constexpr uint32_t kDwellMinMs = 400u;
constexpr uint32_t kDwellSpreadMs = 2600u;

int32_t field_span(int16_t lo, int16_t hi) {
    const int32_t span = (int32_t)hi - (int32_t)lo - kSpriteSize;
    return span > 0 ? span : 0;
}

void choose_target(kf_creature *c, kf_rect field) {
    const int32_t sx = field_span(field.x0, field.x1);
    const int32_t sy = field_span(field.y0, field.y1);
    c->target_x = ((int32_t)field.x0 + (int32_t)kf_rng_below((uint32_t)sx + 1u)) *
                  KF_CREATURE_SUB;
    c->target_y = ((int32_t)field.y0 + (int32_t)kf_rng_below((uint32_t)sy + 1u)) *
                  KF_CREATURE_SUB;
    c->facing = (c->target_x < c->x) ? (int16_t)-1 : (int16_t)1;
}

} // namespace

void kf_creature_init(kf_creature *c, kf_rect field) {
    if (c == nullptr) { return; }
    c->x = ((int32_t)field.x0 + field_span(field.x0, field.x1) / 2) *
           KF_CREATURE_SUB;
    c->y = ((int32_t)field.y0 + field_span(field.y0, field.y1) / 2) *
           KF_CREATURE_SUB;
    c->facing = 1;
    c->dwell_ms = kDwellMinMs;
    c->reaction_hold_ms = 0u;
    c->seen_care_actions = 0u;
    choose_target(c, field);
}

void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }

    if (c->reaction_hold_ms > dt_ms) {
        c->reaction_hold_ms -= dt_ms;
    } else {
        c->reaction_hold_ms = 0u;
    }

    if (c->dwell_ms > 0u) {
        c->dwell_ms = (c->dwell_ms > dt_ms) ? (c->dwell_ms - dt_ms) : 0u;
        return;
    }

    const int32_t step =
        ((int32_t)dt_ms * kSpeedPxPerSec * KF_CREATURE_SUB) / 1000;
    if (step <= 0) { return; }

    bool arrived = true;
    const int32_t dx = c->target_x - c->x;
    if (dx > step)       { c->x += step; arrived = false; }
    else if (dx < -step) { c->x -= step; arrived = false; }
    else                 { c->x = c->target_x; }

    const int32_t dy = c->target_y - c->y;
    if (dy > step)       { c->y += step; arrived = false; }
    else if (dy < -step) { c->y -= step; arrived = false; }
    else                 { c->y = c->target_y; }

    if (arrived) {
        c->dwell_ms = kDwellMinMs + kf_rng_below(kDwellSpreadMs);
        choose_target(c, field);
    }
}

kf_rect kf_creature_bounds(const kf_creature *c) {
    kf_rect r = {0, 0, 0, 0};
    if (c == nullptr) { return r; }
    r.x0 = (int16_t)(c->x / KF_CREATURE_SUB);
    r.y0 = (int16_t)(c->y / KF_CREATURE_SUB);
    r.x1 = (int16_t)(r.x0 + kSpriteSize);
    r.y1 = (int16_t)(r.y0 + kSpriteSize);
    return r;
}
```

Add `#include "kf/rng.h"` to `creature.cpp`.

- [ ] **Step 5: Register and run**

```cmake
    add_test(NAME creature_wander_check
             COMMAND kamiframe-headless --verify-creature-wander)
```

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_wander_check --output-on-failure`
Expected: PASS — `determinism, bounds and movement all hold`.

- [ ] **Step 6: Commit**

```bash
git add hakoniwaos/include/kf/creature.h hakoniwaos/src/creature.cpp simulator/src/headless/headless_main.cpp simulator/CMakeLists.txt
git commit -m "It decides where to go, walks there, and stands about a bit"
```

---

### Task 4: The pet screen takes its pixels back

The ownership switch. This is the task that can break the screen, so it goes in with the erase-then-draw discipline from day one.

**Files:**
- Create: `simulator/src/pet/kf_creature_screen.cpp`
- Create: `simulator/src/pet/kf_creature_screen.h`
- Modify: `simulator/src/sdl/sdl_main.cpp:77` (demo mode)
- Modify: `simulator/src/lvgl/kf_screen_nav.cpp` (route the pet screen to the new drawer)
- Modify: `simulator/CMakeLists.txt`, `ports/esp32/main/CMakeLists.txt` (both builds compile it)

**Interfaces:**
- Consumes: `kf_creature_*` (Tasks 1–3), `kf_blit`/`kf_fill_rect` and the mirrored blit from `kf/blit.h`, `kf_assets_get` from `kf/assets.h`, `kf_pet_session_state()` from `simulator/src/pet/kf_pet_session.h`.
- Produces: `kf_creature_screen_init()`, `kf_creature_screen_frame(uint32_t dt_ms)`.

**Direction, added after this plan was first written (Chris, 2026-08-09).** The
creature faces four ways, served by three sprite sets: `s` (front), `e` (side),
`n` (back). Which one to draw comes from the direction it is currently walking:
moving mostly downward is `s`, mostly upward is `n`, mostly sideways is `e`.
Pick the dominant axis; do not try to blend.

**West is not a fourth sprite set, but it is not always a mirror either.**
Mirroring is a capability, not a rule — some creatures will ship hand-drawn
left-facing art and some will not. So the lookup, when facing west, asks the
pack for the `_w_` name FIRST, and only when `kf_assets_get()` returns null for
it falls back to the `_e_` sprite drawn mirrored. That makes the choice a
property of which PNGs an artist shipped, with no config flag and no code
change per creature — which is where an art decision belongs. A creature with
no west art costs one extra failed lookup per frame; cache the resolved sprite
pointer and only re-resolve when the name would change.

**`kf_creature::facing` already exists** and is maintained by Task 3's wander,
but nothing has used it until now.

**Read before starting:** `docs/architecture/adr-0017-pet-screen.md:143-188` — the black-trail bug that caused `KF_DEMO_NONE`. This task is only safe because the pet screen no longer shares pixels with LVGL; if you find yourself pumping LVGL and blitting in the same frame over the same region, stop and re-read that ADR.

**Dirty-rect budget for this task: 2.** One to erase the creature's previous rectangle, one for its new position. Task 5 adds mess on top and must stay inside the remaining 6.

- [ ] **Step 1: Write the failing test**

A new `run_creature_screen_check()` that runs frames headlessly and asserts the dirty area stays small — the regression that would otherwise creep in silently:

```cpp
static int run_creature_screen_check(void) {
    kf_creature_screen_init();
    size_t worst_rects = 0;
    size_t worst_bytes = 0;
    for (int i = 0; i < 300; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (d.count > worst_rects) { worst_rects = d.count; }
        const size_t bytes = kf_fb_dirty_bytes();
        if (bytes > worst_bytes) { worst_bytes = bytes; }
    }
    /* Two rects: erase the old position, draw the new one. Three 48x48
     * sprite areas' worth of bytes is generous headroom for that. */
    if (worst_rects > 2u) {
        KF_LOGE("creature-screen", "used %zu dirty rects, expected at most 2",
                worst_rects);
        return 1;
    }
    if (worst_bytes > 48u * 48u * 2u * 3u) {
        KF_LOGE("creature-screen", "dirtied %zu bytes in a frame -- too much",
                worst_bytes);
        return 1;
    }
    KF_LOGI("creature-screen", "worst frame: %zu rects, %zu bytes",
            worst_rects, worst_bytes);
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8`
Expected: FAIL — `kf_creature_screen_init` undeclared.

- [ ] **Step 3: Implement the screen**

`simulator/src/pet/kf_creature_screen.cpp`, in outline the implementer fills against the real headers:

```cpp
namespace {
constexpr kf_rect kField = {0, 0, 240, 260}; /* bottom 60px reserved for stats */
constexpr kf_color kBackground = KF_RGB(232, 240, 216);
kf_creature g_creature;
kf_rect g_previous = {0, 0, 0, 0};
bool g_up = false;
} // namespace

void kf_creature_screen_init(void) {
    kf_creature_init(&g_creature, kField);
    g_previous = kf_creature_bounds(&g_creature);
    kf_fill(kBackground);          /* whole screen once, at entry */
    g_up = true;
}

void kf_creature_screen_frame(uint32_t dt_ms) {
    if (!g_up) { return; }
    const kf_pet_state *pet = kf_pet_session_state();

    /* Notice a care action that happened since last frame and start the
     * reaction showing on the body. */
    if (pet->care_actions_taken != g_creature.seen_care_actions) {
        g_creature.seen_care_actions = pet->care_actions_taken;
        g_creature.reaction_hold_ms = 1200u;
    }

    kf_creature_update(&g_creature, kField, dt_ms);

    /* Erase where it was, draw where it is. Two dirty rects, both marked by
     * kf_fill_rect/kf_blit themselves. */
    kf_fill_rect(g_previous, kBackground);

    char name[32];
    kf_creature_sprite_name(pet->stage,
                            kf_creature_pose_for(pet,
                                                 g_creature.reaction_hold_ms),
                            name, sizeof(name));
    const kf_sprite *sprite = kf_assets_get(name);
    const kf_rect now = kf_creature_bounds(&g_creature);
    if (sprite != nullptr) {
        kf_blit(sprite, now.x0, now.y0);
    }
    g_previous = now;
}
```

- [ ] **Step 4: Switch the screen over**

In `simulator/src/sdl/sdl_main.cpp:77`, the pet screen is no longer LVGL's, so the demo mode comment and value must change together — read the surrounding comment before editing and rewrite it to say why the custom engine is now correct here, rather than leaving a comment that contradicts the code.

Route `kf_screen_nav` so the pet screen calls `kf_creature_screen_frame()` and does *not* have an LVGL screen loaded, while the menu screens still do. Do not call `kf_lvgl_port_pump()` on a frame where the creature screen is active.

- [ ] **Step 5: Run every test, not just the new one**

Run: `bash dev.sh test`
Expected: all tests pass, including the pre-existing `pet_screen_check`, `screen_nav_check` and `headless_dirty_area`. If `pet_screen_check` fails, it is asserting on the LVGL circle this task deletes — update that check to assert the new behaviour rather than deleting it.

- [ ] **Step 6: Look at it**

Run: `bash dev.sh run`
Expected: the creature walks around, stands still a while, walks again. No black trails, no flicker.

- [ ] **Step 7: Commit**

```bash
git add simulator/src/pet/kf_creature_screen.cpp simulator/src/pet/kf_creature_screen.h simulator/src/sdl/sdl_main.cpp simulator/src/lvgl/kf_screen_nav.cpp simulator/CMakeLists.txt ports/esp32/main/CMakeLists.txt simulator/src/headless/headless_main.cpp
git commit -m "The pet screen draws its own creature, and it walks"
```

---

### Task 5: The mess it leaves

`poop_count` is a number from 0 to 8 with no positions — Core says so deliberately (`kf/pet.h:349`: "A COUNT, not a list. Where each one sits on screen is presentation"). Placement is this task's job, and it has to fit the dirty budget.

**Files:**
- Modify: `simulator/src/pet/kf_creature_screen.cpp`
- Test: `simulator/src/headless/headless_main.cpp` (extend `run_creature_screen_check()`)

**Interfaces:**
- Consumes: `pet->poop_count`, `kf_creature_screen_frame` from Task 4.
- Produces: no new public API.

**Design note that is the whole point of this task:** eight poops drawn as eight independent dirty rectangles, plus two for the creature, is ten — past `KF_MAX_DIRTY_RECTS` of 8, at which point the framebuffer collapses to one screen-sized box and re-transfers ~31ms of pixels every frame against a 33.3ms budget. So poops must be **static**: drawn once when the count changes, never re-marked dirty on frames where the count is unchanged. They only cost rects on the frames they actually appear or disappear.

- [ ] **Step 1: Write the failing test**

Extend `run_creature_screen_check()` to force poops onto the pet and assert the steady-state cost does not grow:

```cpp
    /* With a full house of mess on screen, a frame where nothing about the
     * mess changed must still cost only the creature's two rects. */
    kf_pet_state *pet = kf_pet_session_state_mutable_for_test();
    pet->poop_count = KF_PET_MAX_POOPS;
    kf_creature_screen_frame(33u);   /* the frame that draws them */
    size_t steady_worst = 0;
    for (int i = 0; i < 100; ++i) {
        kf_fb_clear_dirty();
        kf_creature_screen_frame(33u);
        const kf_dirty_rects d = kf_fb_dirty_rects();
        if (d.count > steady_worst) { steady_worst = d.count; }
    }
    if (steady_worst > 2u) {
        KF_LOGE("creature-screen",
                "%zu rects with 8 poops standing still; they are being "
                "redrawn every frame", steady_worst);
        return 1;
    }
```

If no test accessor for a mutable pet exists, add one guarded by the same `KF_PET_SESSION_ENABLE_DEBUG_TOOLS` flag the existing debug functions use (`simulator/src/pet/kf_pet_session.h:169`), so it compiles out on device exactly as they do.

- [ ] **Step 2: Run it to make sure it fails**

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_screen_check --output-on-failure`
Expected: FAIL — either the accessor is missing, or the count is above 2 because mess is being redrawn every frame.

- [ ] **Step 3: Implement static mess**

Track `g_drawn_poops`. When `pet->poop_count != g_drawn_poops`, repaint the mess band once and update it; otherwise draw nothing. Place poops at fixed slots along the bottom of the field derived from the index — deterministic, so they do not jitter between frames — and keep them clear of the stats band.

The creature walks over them: draw the creature after the mess, and include any poop rectangle the creature's erase rect overlaps in the redraw for that frame, or the erase will punch holes in them.

- [ ] **Step 4: Run the test and make sure it passes**

Run: `cmake --build build -j8 && ctest --test-dir build -R creature_screen_check --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run everything and look at it**

Run: `bash dev.sh test && bash dev.sh run`
Expected: all green; mess appears and the creature walks over it without erasing it.

- [ ] **Step 6: Commit**

```bash
git add simulator/src/pet/kf_creature_screen.cpp simulator/src/pet/kf_pet_session.h simulator/src/headless/headless_main.cpp
git commit -m "Mess on the floor, drawn once and left alone"
```

---

## What this plan deliberately does not do

Each of these is its own plan, and each is genuinely separable:

- **Sleep in Core.** Now fully specified (care-loop spec, "Sleep, settled") but unimplemented: no field, no enum, no night window, no UTC offset. Needs the drowsy state, the automatic plop-down, the optional tuck-in, the deficit, and the neglect-clock pause. Until it exists, `KF_CREATURE_POSE_SLEEPING` is unreachable.
- **Animation.** Every sprite here is a single still (`default_frames = 1`). Chris's bedtime answer requires a drowsy animation, a walk cycle, and the creature putting bedding away — all multi-frame, plus a bedding prop that is not in the manifest at all.
- **The stats band.** Task 4 reserves the bottom 60px and draws nothing in it. Bars and text via `kf/font.h` bitmap text replace the LVGL widgets.
- **Death and evolution scenes.** Both spec'd as scenes, neither built; `KF_CREATURE_POSE_DEAD` currently borrows the sick sprite.
- **Facing** was in this list as unbuilt. It has since moved into scope: a
  mirrored blit landed in `kf/blit.h` as its own task, and Task 4 now selects a
  direction sprite and falls back to a mirrored `e` when no `w` art exists.
- **Multi-frame animation.** Every sprite here is still a single still
  (`default_frames = 1`). Chris has since asked for nine-frame animations on
  essentially every pose, in three directions — the drowsy settle, the walk
  cycle, and the rest. That is a pipeline change (the manifest, the ingest tool,
  and a frame-sequencing player) and a large art-generation spend, and it is its
  own plan. What is built here shows one frame per pose; nothing about it
  prevents a sequence being swapped in later, because the screen resolves a
  sprite by name every time the name changes.
