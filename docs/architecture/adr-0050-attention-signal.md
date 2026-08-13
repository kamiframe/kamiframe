# ADR 0050: The attention signal — `kf_pet_wants()`, `pet.wants()`, and three layers on Home

**Status:** Accepted
**Date:** 2026-08-11

## Context

Task 8 of the screens/clock/sleep plan is the plan's
last task. The plan's own requirements section carried a note explaining
that its original text — "as specified in the answer to question 5" —
pointed at a question that exists nowhere in this repository, and had been
replaced with the substance in full, explicitly flagged as "a starting
point, not a settled design" for Chris to judge on feel. This ADR records
what that starting point resolved to, and where this task disagreed with
it or found it wrong.

The goal: a creature with an unmet need should be legible **across a
room, with no sound** — there is no `kf/hal/audio.h` and no haptic HAL in
this repo (buzzer, speaker and vibration motor are target-spec, not
built), so the signal has to work purely visually, and completely, on its
own.

## Decision

### 1. `kf_pet_want` and `kf_pet_wants()` — a pure query, hysteresis via a caller-held `previous`

`hakoniwaos/include/kf/pet.h` adds:

```c
typedef enum {
    KF_PET_WANT_NONE = 0,
    KF_PET_WANT_FOOD = 1,
    KF_PET_WANT_REST = 2,
    KF_PET_WANT_BATH = 3,
    KF_PET_WANT_FLUSH = 4,
    KF_PET_WANT_PLAY = 5,
} kf_pet_want;

kf_pet_want kf_pet_wants(const kf_pet_state *state, kf_pet_want previous);
```

**No `MEDICINE`.** The plan's own "five questions" section (written
earlier in the same plan document) listed `MEDICINE` in its enum sketch;
Task 8's own requirements list, written later, explicitly overrides that —
"There is no medicine action of any kind... do not invent a `MEDICINE`
want" — since nothing in Core cures sickness directly and no such Lua
action exists (`pet.feed/play/rest/bath/flush` is the complete list). This
is exactly the kind of stale-plan defect `CLAUDE.md`'s "If you are the
operator" section warns about: two sections of the same document
disagreeing, the later one correct. Fixed in the plan (see "What changed
in the plan" below) rather than left for the next reader to trip over.

**Real hysteresis without a new save field — the load-bearing design
choice.** The plan requires both "a pure query... it must not mutate, and
it must not be the thing that decides anything" AND genuine ON/OFF
hysteresis AND "no new save field, no new state to migrate." Those three
cannot all hold if the function's only input is the current
`kf_pet_state`: hysteresis is definitionally not a function of the
instantaneous value alone — a need sitting between the two thresholds
means something different depending on which direction it arrived from,
so *something* has to remember what was true a moment ago.

Every other stateful signal in `kf/pet.h` (`kf_pet_state::sick` is the
clearest example) keeps that memory as a stored, **saved** bit. This one
deliberately does not. Instead, `kf_pet_wants()` takes the caller's own
last answer back in as `previous` — the same "previous value in, fresh
value out" shape `sdl_debug_window.cpp`'s own `previous_pressed` uses for
edge detection, except here the caller is
`kf_pet_session_wants()` (`simulator/src/pet/kf_pet_session.{h,cpp}`),
which holds exactly one `kf_pet_want` in ordinary session memory — never
saved, never migrated, reset on `kf_pet_session_init()` and the two
DEBUG-gated functions that fabricate a fresh state
(`_debug_reset()`/`_debug_jump_to_stage()`), for the identical reason
those already reset the debug snapshot ring: a fresh session or a
fabricated jump has no real "was already asking" history to carry
forward. `kf_pet_wants()` itself never stores anything and never mutates
`state` — call it twice with the same two arguments and it returns the
same answer both times, which is what "pure" means here, and is checked
directly (see "The proof" below).

Only `previous`'s own want gets the generous OFF threshold; every other
want is evaluated against its plain ON value, since a fresh crossing into
"wanting" needs no history to be trusted.

### 2. The five wants, and the four not-quite-matching Core enums

The wants map onto exactly the five things a player can do:
`FOOD` (`pet.feed`), `REST` (`pet.rest`), `BATH` (`pet.bath`), `FLUSH`
(`pet.flush`), `PLAY` (`pet.play`). `FLUSH` deliberately sits outside
`kf_pet_care_action` (`KF_PET_CARE_ACTION_COUNT` is 4) — the plan's own
instruction not to fold it in to make the mapping tidier, matching
`kf/pet.h`'s existing reasoning for why flushing is a chore with no
opinion, not a fourth care action.

### 3. Hysteresis thresholds — named constants, not `kf_pet_config` fields

One `_ON`/`_OFF` pair per want, all in `kf/pet.h`, compile-time constants
in the same style as the existing `KF_PET_DIRTY_FLIES_MP`/`STINK_MP` —
not `kf_pet_config` fields, because unlike decay rates and stage
durations, when a creature starts *asking* is not something this slice
expects a cartridge author to re-tune per pet.

| Want | ON | OFF | Direction |
|---|---|---|---|
| FOOD | `hunger_mp` ≤ 25000 (25%) | ≥ 40000 (40%) | lower is worse |
| REST | `energy_mp` ≤ 25000 | ≥ 40000 | lower is worse |
| PLAY | `happiness_mp` ≤ 25000 | ≥ 40000 | lower is worse |
| BATH | `dirtiness_mp` ≥ `KF_PET_DIRTY_STINK_MP` (80000) | ≤ `KF_PET_DIRTY_FLIES_MP` (50000) | higher is worse |
| FLUSH | `poop_count` ≥ 3 | ≤ 1 | higher is worse |

**Argued with the plan's own alternative framing, one sentence of
reasoning.** The plan's earlier "five questions" section separately floats
"needs attention" at 70% as existing design language (`kf_pet_default_
config()`'s own comment, from the demand-curve doc). Firing the
disruptive, wander-stopping signal that early — every ~27 real minutes for
a baby — would make it background noise rather than a genuine attention
signal, so FOOD/REST/PLAY's ON instead matches `creature.lua`'s own
`classify()` "low" band (25–30%, already carrying the "starting to get
hungry"/"could use some playtime"/"getting a little tired" log messages),
which is the more conservative, already-shipped signal for "this actually
needs attention soon." BATH and FLUSH reuse existing named constants and
plain integer-count reasoning respectively, both explained in `kf/pet.h`'s
own comment on the thresholds. **All six numbers are exactly what the
plan called them — a starting point for Chris to judge on the board, not
a settled design.**

Priority order, unchanged from the plan's own starting point: **FOOD,
REST, BATH, FLUSH, PLAY**. Kept rather than switched to a severity-based
tiebreak — the plan itself frames that as "a defensible alternative," not
a requirement, and a fixed order is simpler for a script author or a
player to learn once.

**No want fires while dead or asleep**, unconditionally, checked first.
Task 6 (ADR 0048) landed specifically so the asleep half is expressible;
the dead half matches every existing care action's own
`if (state->dead) return;` guard.

### 4. `pet.wants()` — an uppercase string, or `nil`, never the raw enum

`sdk/lua/kf_lua_port.cpp` adds `pet.wants()`, wrapping `kf_pet_session_
wants()` and converting the enum to `"FOOD"`/`"REST"`/`"BATH"`/`"FLUSH"`/
`"PLAY"` or `nil` — the audience constraint this whole SDK is written
against (`CLAUDE.md`: "a WordPress developer or a jQuery developer"): a
script writing `if pet.wants() == "FOOD" then` never has to learn what a
numbered enum means, matching `pet.stage()`'s existing convention rather
than the raw-index convention `pet.teen_form()`/`pet.base_trait()` use for
genuinely-undecided creative content.

### 5. The `!` glyph — the font, and a second call site the plan's own note didn't name

`hakoniwaos/src/font_data.h`'s `0x21 '!'` row was genuinely all-zero, as
the operator's brief said. Fixed at the source: `tools/make_font.py`'s
`GLYPHS` gains a five-row bar over a single dot (matching `GLYPHS["."]`'s
own dot on the last row), regenerated with `python3 tools/make_font.py >
hakoniwaos/src/font_data.h`.

**One thing the brief's font-glyph note did not mention, found while
implementing:** `sdk/lua/kf_lua_scene.cpp`'s `char_supported()` keeps its
own, *separate* copy of the supported-punctuation set
(`kSupportedPunctuation`, `".,:-/%+()"`), used only to decide when to log
"the font has no glyph for this character." Adding the glyph to the font
without also adding `!` here would have shipped a real, drawable glyph
that still logged a false "no glyph" warning every time a script used it —
harmless to rendering, but a wrong diagnostic message that would have
outlived this task. Fixed in the same commit; both `kf/font.h`'s and
`kf_lua_scene.cpp`'s header comments (which each separately restate the
supported character set in prose) are updated to match.

### 6. The three presentation layers — `creature.lua` only, no new Core drawing

All three are scene setters inside `on_home_frame()`'s existing
not-tucked-in branch, gated on `local want = pet.wants()`:

1. **Pose and position.** `body:sprite(want_stage_token() .. "_objecting_s")`,
   `body:frame(0)` (objecting is single-frame for every creature in the
   pack), `body:move(96, 212)` — horizontally centred (matching the
   shrine's own convention) at the wander field's bottom edge (`KF_CREATURE_
   PRESENTER_FIELD` is `{0,0,240,260}`), closest to the viewer. The
   underlying C++ wander (`kf_creature_presenter_advance()`) keeps
   advancing unconditionally every frame regardless — the same "moves
   invisibly underneath" shape the tucked-in futon already established —
   so nothing here touches the presenter itself.
2. **The pulsing `!`.** One text object (`want_bang`), shown for 500 ms of
   every 1000 ms while a want is active, reset to a fresh visible-first
   cycle the frame a want first appears.
3. **The inverted care-guide entry.** `paint_guide(want)` inverts exactly
   the guide slot naming the wanted button, via the same
   `kf_scene_set_colors()` trick the Settings cursor already uses — the
   five guide labels are now kept as named locals (`guide_labels`) instead
   of thrown away after their declaration loop, purely a receiver change,
   same layout and object count otherwise (plus the one new `want_bang`
   object).

`want_stage_token()` reimplements `kf_creature_sprite_name()`'s
`<stage><branch>` token in Lua (`"egg"`/`"baby"`/`"child"`/`"teen<N>"`/
`"adult<N><M>"`) rather than exposing that C++-private helper — the same
"what a stage/branch number means is not Core's business" line `kf/pet.h`
already draws. Never actually exercised for `stage == "egg"` in practice:
eggs never decay (`kf_pet_default_config()`'s EGG row is all-zero rates),
so `pet.wants()` can never return non-nil for one; the pack also has no
`egg_objecting_*` art, matching the same "no real art, defensive default
only" situation ADR 0049 already recorded for adult sleeping art.

### 7. `screen_parity_check` — the divergence this task creates, named and bounded rather than silently accepted or hidden

`run_lua_vs_cpp_screen_check()`'s existing synthetic timeline sets
`poop_count = KF_PET_MAX_POOPS` at frame 150, specifically to prove all 8
poop-slot boxes render identically in both Home implementations. That
value is now **also** enough to cross `KF_PET_WANT_FLUSH_ON_POOPS` — the
Lua Home screen responds with the attention signal, which
`kf_creature_screen.cpp` (the frozen C++ reference/fallback screen this
task's own requirements never ask to touch — "no new Core drawing," and
this file is not Core, but it is also not part of Task 8's scope) has no
notion of at all. This is structurally unavoidable: there is no poop count
that is simultaneously "at the max, proving all 8 slots render" and
"under the flush threshold." ADR 0049 recorded an analogous
situation for the tucked-in futon, but that check's timeline never sets a
wall clock, so it never actually exercises sleep and the two screens
simply never diverge over it — this timeline cannot dodge FLUSH the same
way without losing the max-poop-count coverage.

Rather than lose that coverage or let the check start silently asserting
less than it claims to, the divergence is named: strict byte parity is
required for frames 0–149 exactly as before, and frame 150 is now
asserted as the exact first-divergence point (not merely "at or after"),
with a distinct failure message either if parity breaks *earlier* than
that (a real regression, over behaviour the two screens are still meant
to share) or if the two screens stay byte-identical *past* frame 150 (the
attention signal silently stopped firing). See `headless_main.cpp`'s
`run_lua_vs_cpp_screen_check()` for the exact comment and constant
(`kScreenParityWantsBeginFrame`).

`kSettingsCheckExpectedObjectCount` (the measured Home+Info+Settings
live-object count) moves from 45 to 46 — the one new `want_bang` object.

### 8. What defers to hardware — the same one call site the plan named

No `kf/hal/audio.h`, no haptic HAL, unchanged by this task. When sound or
haptics arrive, they hang off the identical `KF_PET_WANT_NONE →
something` transition `creature.lua`'s `want`/`was_wanting` local already
computes every frame — one call site, already isolated. No sound API is
designed here, matching the plan's own instruction to stop at naming the
hook.

## The proof

- `ctest --test-dir build` — **49/49** (the prior 48 plus the new
  `attention_signal_check`).
- `python3 tools/check_no_heap.py .` — `core is heap-free (36 files
  scanned)` — unchanged file count; this task added no new `hakoniwaos/`
  source files, only new declarations/functions inside existing
  `pet.h`/`pet.cpp`.
- No `float`/`double` introduced in any `hakoniwaos/` file this task
  touched (`pet.h`, `pet.cpp`), checked by hand — `check_no_heap.py` does
  not scan for floats (`kf/scene.h`'s own comment says so).
- Worst-case dirty rects with a want active: **1** (`KF_MAX_DIRTY_RECTS`
  is 8), measured over 300 real-cadence frames by `attention_signal_
  check`'s own Part C8 — the same "erase and draw overlap and get
  unioned" reasoning ADR 0049 gives for its own measurement: the held
  pose, the `!`, and the inverted guide entry sit close enough together,
  and change together often enough, that the differ collapses them into
  one rectangle almost every frame.
- The ESP-IDF cross-compile (`-DKF_PANEL=ili9341`) is clean — zero
  warnings in project code. Firmware image: 526,448 bytes (67% of the app
  partition free; ADR 0049 recorded 519,888 bytes before this task, so
  the attention signal costs roughly 6.4 KB).

### Non-vacuity: every new assertion broken and watched fail, then restored

Per `CLAUDE.md`'s "a test that passes is not evidence it tests anything,"
every new assertion below was broken, run, its exact failure message
recorded, then restored and the full suite re-confirmed green:

| Assertion | How it was broken | Exact failure message |
|---|---|---|
| Hysteresis (the plan's named one): a need hovering in the dead zone, with the want already active, does not flip on consecutive calls | Collapsed `KF_PET_WANT_FOOD_OFF_MP` to equal `KF_PET_WANT_FOOD_ON_MP` (the plan's own suggested check) | `FAILED: hunger hovering in the dead zone between ON and OFF, with FOOD already active, stays FOOD on two consecutive calls -- the hysteresis assertion` |
| Priority order: FOOD wins over REST when both are unmet | Swapped the `if (food)`/`if (rest)` order in `kf_pet_wants()` | `FAILED: priority: FOOD wins when every want is active at once` |
| Dead/asleep gating | Replaced the `if (state->dead \|\| state->asleep)` guard with `if (false)` | `FAILED: a dead creature wants nothing, even mid-hysteresis` and `FAILED: an asleep creature wants nothing, even mid-hysteresis -- Task 6 (ADR 0048) landed specifically so this is expressible` |
| Purity: `kf_pet_wants()` does not mutate `state` | Added a stray `hunger_mp += 1` inside the function | `FAILED: kf_pet_wants() does not mutate state -- hunger_mp unchanged after two calls` |
| Feeding repeatedly eventually clears FOOD (bounded loop) | Disabled the feed loop (`while (false && ...)`) | `FAILED: feeding repeatedly eventually clears FOOD (within 20 feeds -- kf_pet_feed() clamps at KF_PET_MILLIPERCENT_MAX so this is a hard bound, not a hopeful one)` |
| `pet.wants()` Lua binding returns `"FOOD"` for a hungry creature | Made the `KF_PET_WANT_FOOD` case push `nil` instead | `FAILED: pet.wants() reads "FOOD" (reported as 1) for a hungry creature` (plus three real-screen assertions downstream also correctly failed, since they all depend on the same binding) |
| The held pose/position actually draws at (96,212) | Removed `body:move(kWantPoseX, kWantPoseY)` from the want branch | `FAILED: wanting: something is drawn at the front-centre held pose position (96,212)...` |
| The FEED guide entry actually inverts | Made `paint_guide()` a no-op | `FAILED: wanting FOOD: the "1:FEED" guide entry is inverted -- its own background fill no longer matches Home's background` |
| The `!` actually blinks, not a static mark | Forced `want_bang:show()` unconditionally | `FAILED: wanting: the "!" is hidden later in the SAME 1000 ms cycle -- it is actually blinking, not a static mark` |
| The `!` is genuinely drawn (the font glyph is load-bearing) | Zeroed `font_data.h`'s `0x21 '!'` row back out | `FAILED: wanting: the "!" is visible partway through its own blink cycle (just became active, so this samples the visible half)` |

Two early pixel-sample coordinates in `attention_signal_check` (the guide
entries, the `!` glyph) were themselves caught as vacuous on first write —
sampling the label's un-drawn left margin, and the glyph's blank margin
column, both of which read the same background colour whether the object
was inverted/shown or not. Fixed to sample inside the label's actual
bounding box and the glyph's own lit column before any of the above
breakage runs were performed.

## What changed in the plan

the screens/clock/sleep plan's Task 8
section is updated to record what landed (this ADR, the actual thresholds
chosen, the `MEDICINE` contradiction between its own "five questions"
section and its Task 8 requirements list, and the `kf_lua_scene.cpp`
second call-site finding) rather than left as the pre-implementation
starting point.

## Not verified

Nothing here has been run on real hardware — the owner is away from the
bench, per this task's own instruction not to flash. The ESP-IDF
cross-compile is clean, which is as far as this task goes.
