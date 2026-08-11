# Screens, the Clock, Sleep, and the Attention Signal — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Four things, in the owner's order. (1) The Info screen is declared in
Lua, proving the retained scene carries a screen that is not the creature.
(2) A global Settings screen with a real system clock — 12-hour with am/pm,
editable with the seven buttons, saved, and proved to survive a power cut on
the DS3231's coin cell. (3) Sleep, as the care-loop spec settles it. (4) The
pet gains a way to *ask* for something, which is the most Tamagotchi-defining
thing it currently cannot do.

**Architecture:** The clock comes before sleep because sleep's night window is
wall-clock — 22:00 to 07:00 local — and a night window on a clock nobody
trusts is a bug generator. Screens come before the clock because the Settings
screen is where the clock is edited, and three screens is where the current
two-entry C++ registry stops being the right shape. Each task retires one
named risk; the branch is green after every one.

**Tech Stack:** C++17, C for the Core modules, Lua 5.5.0 (fetched,
`cmake/fetch_lua.cmake`), CMake + ESP-IDF v6.0.2, CTest via
`kamiframe-headless --verify-*` check modes, `tools/kf_debug.py` over UART at
115200 for the bench work.

## Status: Tasks 1–4 and **6** landed; Task 5 partly done; Tasks 7–8 not started

**Updated 2026-08-11.** Tasks 1, 2, 3 and **4** (Lua time API, Settings
screen with an editable 12-hour clock) have landed and Task 4 is confirmed
working on hardware by its owner. **Task 5's core risk is retired** — the
DS3231 kept time across a real power cut on its coin cell, measured at the
bench; see the STATUS block on Task 5 itself for the numbers and for the
three sub-requirements that were *not* done (`KFDBG RTC`, the
cell-removed negative case, and on-device confirmation that the pet actually
ages across the gap).

**Task 6 (sleep in Core) has now landed too, entirely Core-side** — see
ADR 0048. `kf_pet_advance()` now carries `state->last_advanced` forward
during live play (its own dedicated check, landed and green before any
sleep logic was written, per this task's own instruction); `bool asleep`
joined `kf_pet_state` (save version 9, `KF_PET_SAVE_BYTES` 92); night is
22:00–07:00 via `kf_clock_seconds_in_daily_window()`; falling asleep is
automatic and the live/offline rules are the identical code path; the
neglect clock pauses while asleep (needs keep decaying); the waking
fraction (15/24) compresses `sickness_onset_seconds`/`sickness_death_
seconds`, never the accrual rate; `hokorimaru_check` passes unmodified;
offline stays analytic (no stepping, even across several nights); the egg
does not sleep (a decision, not a gap); and `kf_creature_pose_for()` can
now return `KF_CREATURE_POSE_SLEEPING`, sitting between `sick` and the held
reaction in precedence. 47/47 tests green (46 plus `pet_sleep_check`; the
pre-existing `pet_offline_ageing_check` also grew night-spanning cases).
**Tasks 7 and 8 have not started** — both need Chris to judge feel on the
board, per the "Who has to be at the bench" table below, and neither is
blocked on anything but Task 6, which is now done.

Written 2026-08-13 as NOT STARTED. Since then: Task 1 (`kf.screen()` groups,
Info declared in Lua — `d3354cf`, `83c140e`), Task 2 (`KF_ENABLE_LVGL`
default OFF, Info's LVGL screen deleted — `f3ddbc8`, `27a6649`), and further
work (`df9315b`) have landed. The task checkboxes in this
document were not updated when the work landed — treat the checkbox state
below as unreliable and verify against the tree, not the boxes. Every figure
in "What is true today" was measured in this worktree on this machine on
2026-08-13 by running the command named next to it — not copied from an ADR,
and not carried over from the brief that commissioned this plan (three of
that brief's figures turned out to be wrong; see "Six things the premise got
wrong") — but several have since drifted as Tasks 1 and 2 landed; corrections
are noted inline below rather than silently re-measured, since this table's
whole point is to be checked against the tree, not trusted.

---

## READ THIS BEFORE DISPATCHING ANY TASK FROM THIS PLAN

**This project's plan documents have manufactured six defects by being copied
verbatim.** Three were comments contradicting their own code; one was a real
`ValueError` in a listing; one was a function called four times against an
assert that fires on the second call, which cost two implementers time; one
was a requirement to build and persist a config field (this plan's clock
storage key — see "What is true today" and Task 4) that a previous decision
had deliberately decided not to implement. See the identical banner on
`2026-08-12-lua-game-layer.md` and `2026-08-10-animated-indexed-sprites.md`,
and `CLAUDE.md`'s "If you are the operator" section.

This plan therefore keeps code listings **minimal and load-bearing only**.
Where an exact snippet is not required for correctness, the step states the
requirement and names the file, and the implementer writes it. Two listings
appear — the settings-screen Lua sketch and the `kf/clock.h` function list —
and both have been reasoned line by line against names verified in the tree.

Two rules follow, and the second matters more:

1. When a review finds a bad comment or pattern, grep **this file** as well as
   the source tree. A defect here costs one defect *per remaining task*.
2. **Update this plan when a decision is made, before dispatching the task it
   affects.** Briefs are generated *from* this file, so a stale line here is
   re-served to every implementer that follows.

**One extra rule for this plan specifically.** Task 5 ends at a physical
unplug, and hardware has no assertion to fail — only a number that is wrong.
That task states what must be *seen*, and "it probably kept time" is not a
result. Record what was actually observed in its ADR.

---

## What is true today, measured in this worktree on 2026-08-13

| Claim | Verified how | Result |
|---|---|---|
| Desktop suite green | `ctest --test-dir build -N` | **44 tests**, `#1 headless_determinism` … `#44 lua_embed_check` |
| The clock setter exists | `hakoniwaos/include/kf/hal/time.h:55` | `kf_result kf_time_set_wall(int64_t epoch_seconds)` |
| The device backend writes through to the DS3231 | `ports/esp32/hal/esp_time.cpp:311` | Sets the RAM clock via `settimeofday()`, then writes the seven time registers and clears the OSF bit. Best-effort: a write failure logs and keeps the RAM clock |
| Nothing outside tests calls it | `grep -rn kf_time_set_wall hakoniwaos simulator ports sdk tools` | Three call sites, all in `simulator/src/headless/headless_main.cpp` (`:435`, `:733`, `:1013`). The comment at `esp_time.cpp:34` that says so is **accurate** |
| The desktop backend implements it properly | `simulator/src/host/host_time.cpp:80` | Adjusts `g_wall_offset` against the simulated clock. Works, and is what the headless checks exercise |
| There is no Lua time binding | read all three `luaL_Reg` tables | `kf` = `log`, `report`, `home_screen_active` (`sdk/lua/kf_lua_port.cpp:112`) plus ten drawing/screen entries including **`screen`, added by Task 1** (`kf_lua_scene.cpp:861`); `pet` = **23**, `pet.stage_seconds()` added since (`:366`); `creature` = 5 read-only (`:447`). **Nothing exposes a clock still** |
| `kf/scene.h` has no clipping primitive | read the header | `kf_clip_push()/pop()` is named at `:136` as an ADR 0040 follow-up that was **not built**. Fixed lists and icon rows are fine; a scrolling list of arbitrary length is not |
| Text is the bitmap font | `hakoniwaos/include/kf/font.h:34-37` | `KF_FONT_GLYPH_W/H` 5x7 in a `KF_FONT_CELL_W/H` 6x8 cell. Uppercase only |
| Screen navigation is C++ | `wc -l simulator/src/pet/kf_screen_nav.cpp` | **Moved and grown since Task 1 landed: 228 lines, at `simulator/src/pet/` (no longer `simulator/src/lvgl/`, no longer `#include <lvgl.h>` — ADR 0044/0045). Register-by-name, `g_screens[KF_SCREEN_NAV_MAX_SCREENS]`, `KF_SCREEN_NAV_MAX_SCREENS = 8`, not the old fixed two-entry array |
| The SDL debug window depends on it | `sdl_debug_window.cpp:129`, `:428`, `:862` | "Next Screen" button → `kf_screen_nav_debug_advance()`; readout → `kf_screen_nav_name(kf_screen_nav_debug_index())` — the local `screen_name()` switch Task 1 was meant to replace is gone, queries the registry directly now |
| LVGL's arena | `hakoniwaos/include/kf/budget.h:195`, device boot log | `KF_ARENA_LVGL_BYTES` = `256u * 1024u`; the board logs `handing LVGL 262144 bytes from KF_ARENA_LVGL` |
| LVGL's footprint in the tree | `ls simulator/src/lvgl/`, `simulator/CMakeLists.txt:135-144` | **Shrunk since Task 1/2 landed: 18 files** in that directory (not 27); `kamiframe_lvgl_port` compiles **8 `.cpp` files**, all genuinely LVGL code now — `kf_screen_nav`, `kf_lua_home_screen` and `kf_error_banner` are no longer in this list, having moved out with Task 1/2 |
| Info is a pure widget tree | — | **No longer true: `kf_pet_info_screen.cpp`/`.h` are deleted (Task 2, ADR 0045).** Info is a `kf.screen("info")` group in `creature.lua` now, not an LVGL widget tree |
| The old LVGL Home is unreachable | `kf_screen_nav.h:94-102` (moved from `:37-47`), `grep kf_pet_screen` | `kf_pet_screen.cpp` (**364 lines**, unchanged) is initialised by nothing in a running build. Its only remaining caller is `run_pet_screen_check()`, gated on `KAMIFRAME_PET_SCREEN_GOLDEN_CHECKSUM` = `132458f0171a2c0b` (`simulator/CMakeLists.txt:618`, not `:566`) |
| Core has a sleep field now | `grep -i asleep hakoniwaos/include/kf/pet.h` | **Landed by Task 6, ADR 0048.** `bool asleep` joined `kf_pet_state`, computed by `apply_stage_segment()` against `kf_clock_seconds_in_daily_window()`. (Was: nothing but `kf_power_deep_sleep_until()` references, before Task 6.) |
| `KF_CREATURE_POSE_SLEEPING` is reachable now | `hakoniwaos/include/kf/creature.h:32`, `hakoniwaos/src/creature.cpp`'s `kf_creature_pose_for()` | **Landed by Task 6, ADR 0048.** `kf_creature_pose_for()` returns it when `pet->asleep`, between `sick` and the held reaction. (Was: unreachable, with a header comment saying so, before Task 6 — that comment was corrected in the same commit that made it false.) |
| Sleeping art in the shipped pack | `find examples/creature_demo/sprites -name '*sleeping*'` | **18 files**, one frame each: `{baby,child,teen0..3}_sleeping_{s,e,n}_01.png`. **No `egg_sleeping`, no adult family at all** |
| The attention signal | grep for any want/demand/alert query | **Nothing.** `pet.*` exposes the raw needs and the player reads the bars |
| Save format | `hakoniwaos/src/pet.cpp:79`, `kf/pet.h:617` | `kSaveVersion = 9`, `KF_PET_SAVE_BYTES = 92`, key `"pet"`, against `KF_STORE_MAX_VALUE_BYTES` = 4000. **Bumped by Task 6** (was version 8 / 91 bytes) to add `asleep`; a version-8 save is refused, per ADR 0048 |
| Hold-to-repeat is available | `hakoniwaos/include/kf/app.h:135-136` | Both `kf_app_buttons_held()` and `kf_app_buttons_pressed()` exist |
| `KFDBG STATE` carries no clock | `handle_state()`'s format string, `kf_dbg_bridge.cpp:238-247` | 28 keys, none of them wall-clock or RTC. The reply buffer is sized by a documented byte computation at `:228` |

### Six things the premise of this work got wrong or did not know

Three of these came from the brief that commissioned this plan. Two of the
remaining three change a task's shape.

**1. There are 18 sleeping sprites, not "roughly 140 generations".** Six stages
times three directions, **single-frame each** — there is no `_02`. Two
consequences the sleep tasks have to live with: a sleeping creature will be
**static**, with no breathing loop, unless art is generated; and **the egg has
no sleeping pose at all**, so Core's sleep state must be defined for a stage
whose art cannot show it. There is also no `adult_*` family in the pack
whatsoever, which is a pre-existing gap this plan does not fix but which sleep
must not trip over.

**2. `last_advanced` only moves when a save is loaded, so during live play Core
does not know what time it is.** `hakoniwaos/src/pet.cpp:1229` sets it inside
`kf_pet_load_and_advance()`; `kf_pet_advance()` never touches it. The care-loop
spec already identified this and prescribed the fix — *"for `kf_pet_advance()`
to carry `last_advanced` forward by the same elapsed seconds that drive decay:
wall time then enters Core exactly once, at load"* — and this is the single
largest hidden dependency in the whole plan. **Landed as Task 6's first step,
exactly as prescribed** — see ADR 0048. `kf_pet_advance()` now carries
`last_advanced` forward internally (a local cursor updated at each segment,
written back to `state->last_advanced.epoch_seconds`), with its own dedicated
check proving it before any sleep logic was written.

**3. `kf_screen_nav.cpp` is 184 lines and lives inside the LVGL library.**
`simulator/CMakeLists.txt:133` compiles it into `kamiframe_lvgl_port`, and it
`#include <lvgl.h>`. So the file that owns navigation *is* the file that
couples navigation to LVGL. That is why Task 1 and Task 2 are ordered the way
they are: you cannot get LVGL out of the app without moving that file, and you
cannot move that file sensibly while it still has an LVGL screen to load.

**4. A sprite background costs a full-screen repaint on any dirty frame.**
`kf/scene.h:126-137` says so plainly: `kf_blit_frame()` cannot clip to an
arbitrary rectangle, so `kf_scene_commit()` falls back to redrawing everything
whenever a sprite background is set and anything at all is dirty. **The
Settings screen must use `kf_scene_set_background_color()`, not a sprite**, or
a ticking clock costs ~31 ms of a 33.3 ms frame every second. Named here
because "put a nice background on the settings screen" is exactly the instinct
that would do it.

**5. There is one scene, not one scene per screen.** `KF_SCENE_MAX_OBJECTS` is
64 and `hakoniwaos/src/scene.cpp` holds a single file-static table. Three
screens therefore share 64 slots. Counted from the source:
`examples/creature_demo/creature.lua` declares **24** (1 body, 1 shrine, 8
poops, 3 tracks, 3 fills, 3 labels, 5 guide entries) and
`kf_error_banner_create()` adds **1**, so Home is **25**. Info as text objects
is **8**. Settings is budgeted at **14** below. That is **47 of 64**, which
fits with headroom on paper — but it is not obviously true, and nothing
verifies it yet: `run_screen_group_check()`'s live-object assertion (risk 5)
checks a synthetic two-screen, two-object fixture, not this arithmetic
against the real three screens. Treat 47-of-64 as unverified until a check
actually runs Home, Info and Settings together and counts. Do **not** raise
`KF_SCENE_MAX_OBJECTS` to make room: the Task 3 report measured **224 bytes per
object, landing in `.data` rather than `.bss`**, so 64→96 would cost roughly
7 KB of flash *and* 7 KB of RAM. Fix the `.data` defect first if space is ever
genuinely short.

**6. `kf_time_set_wall()` is not the untested part — the DS3231 write is.** The
desktop backend (`host_time.cpp:80`) is exercised three times in
`headless_main.cpp`, so the *contract* is proved. What has never executed is
`esp_time.cpp:311`'s seven-register I2C write and OSF clear, on silicon, with a
coin cell. That distinction decides what Task 5 has to test and what it can
take for granted.

### Stale comments found while reading — defects by this codebase's own rule

Each task that touches the file fixes its own. The rest are listed so nobody
rediscovers them.

Three of the original four entries here are fixed as of Task 1 and Task 2
landing and have been pruned: `kf_screen_nav.h`'s stale "Home
(kf_pet_screen.cpp) or Info" line (the file, now at `simulator/src/pet/`, no
longer even mentions `kf_pet_screen.cpp`), `kf_pet_info_screen.h`'s same
staleness (the file is deleted), and `sdl_debug_window.cpp`'s
`screen_name()` duplication (deleted; it now calls `kf_screen_nav_name()`
directly). One remains open:

- `tools/kf_panel.py:630-633` — `STATE_FIELD_ORDER` still names `hunger`,
  `happiness`, `energy`, `time_in_stage_s`, `frame_time_ms`, `free_heap_bytes`
  where the firmware emits `hunger_mp`, `happiness_mp`, `energy_mp`,
  `stage_elapsed_s`, `frame_us`, `heap_free_internal`. Harmless (unknown keys
  are appended), still wrong. Carried over unfixed from
  `2026-08-12-lua-game-layer.md:148-154`. **Out of scope again; noted so it is
  not mistaken for damage this plan did.**

### Found in passing, not a defect this plan fixes

`examples/creature_demo/creature.lua`'s `on_frame` hides the body and shows the
shrine when `pet.dead()`, but the eight poop boxes are only updated inside the
`else` branch — so a creature that dies with mess on screen keeps that mess
visible under the shrine forever. **No comment says whether that is
deliberate.** It may well be (it died in its own filth, which is the point), but
an uncommented ambiguity in the reference script third parties will copy is
worth one line of comment either way. Cheap; assign it to whichever task next
edits that file.

---

## Decisions already taken — record, do not re-litigate

**Retained mode.** Lua declares what exists; the engine diffs and computes
dirty rectangles. `KF_MAX_DIRTY_RECTS` is 8, the current worst case is 3. **No
game author should ever meet a dirty rectangle**, and no API added by this plan
may make one visible.

**The pet simulation stays in Core.** `CLAUDE.md` puts needs, decay and
evolution in `/hakoniwaos`. **Sleep is a Core feature** — it changes decay,
neglect accrual and offline fast-forward, all of which are Core's. **How sleep
looks is the game's** — pose, bedding, the drowsy cue, all declared in
`creature.lua`. The attention signal splits the same way: *what the pet wants*
is Core, *how it asks* is the game.

**The audience is a hard requirement.** *"Easy to use for non-hardware devs.
Like a WordPress developer or jQuery developer would not have too much
trouble."* Every API in this plan is judged against that, and the demo scripts
are the reference third parties will copy. The jQuery accessor convention holds
throughout: **no argument reads, arguments write.**

**Lua comments ship to the device and are parsed at boot.** Task 1 of the Lua
plan measured a 3.4 KB comment header costing 3.4 KB of flash. Keep script
comments tight; long reasoning goes in the C++ binding or an ADR.

**The clock is a device setting, not a per-creature one.** The care-loop spec
settled this: *"Local time is set once in the device's global settings and
every creature on it shares that. Core carries a UTC offset from config, and
nothing per-egg."* That is why the clock lives on a **global Settings screen**
not in the pet save — the "device setting, not per-creature" half of this
still holds exactly as written. The "UTC offset from config" half is
superseded: see "Timezone: settled by Chris" below, which the spec predates.
There is no offset — the RTC holds local time directly, and Task 4 persists
nothing at all beyond the RTC itself: no storage key, "ready for" internet
sync or otherwise (see Task 4's own note against adding one).

---

## The five questions this plan was required to answer

### 1. Does screen navigation move to Lua? — Partly. The registry stays C++; screens become a Lua concept over it.

**The decision:** `kf_screen_nav.cpp` remains the single owner of "which screen
is showing". Lua gains `kf.screen(name)` — a named **group of scene objects**
— and `screen:show()` routes through the C++ registry rather than around it.
The registry stops being a fixed two-entry array and becomes a small
register-by-name table that Lua populates at script load.

**Why not move it wholesale.** Three things read the registry and none of them
can read a Lua table: `sdl_debug_window.cpp`'s "Next Screen" button (`:444`),
its screen readout (`:870`), and `kf_screen_nav_wants_lvgl()`, which both
`sdl_main.cpp` and `ports/esp32/main/app_main.cpp` use to decide whether to
pump LVGL at all. Moving navigation into the script means inventing a C-callable
shim back out of Lua for each of them — three new couplings to buy one fewer.
It also means the debug window's screen list depends on a script having loaded
successfully, so a script error would take the debug UI down with it.

**Why not leave it entirely alone either.** `kScreenCount = 2` is a
compile-time constant and `screen_name()` is a hardcoded switch with a `"?"`
fallback. A third screen makes both wrong. Register-by-name is a smaller change
than either extreme and it makes the debug window's readout correct by
construction instead of by remembering to update a switch.

**What each costs, stated plainly.** Registry-in-C++ costs one new export
(`kf_screen_nav_register()`) and one behaviour rule the script author must not
break (`screen:show()` is the only way to switch — see Task 1's design note).
Registry-in-Lua would cost roughly a day of shim work, a debug window that goes
blank on a script error, and it would put screen switching behind the very
`lua_pcall` boundary that is meant to keep a bad script from breaking the
device. Not worth it.

### 2. What does the time API look like? — Four functions, no epoch, no `struct tm`.

```
kf.time()          -- "9:05 AM", ready to draw. The only call most scripts need.
kf.hour()          -- 0..23, integer, local
kf.minute()        -- 0..59
kf.clock_set()     -- true once the clock has been set; false on a fresh device
```

Writing is deliberately **not** part of the same surface. Setting the clock is a
device-settings action, not a game action, so it is `kf.set_clock(hour, minute)`
and it is documented as "the Settings screen calls this; your game almost
certainly should not". It takes two integers, applies today's date itself, and
returns `true`/`false` — the script never sees an epoch second.

The `int64_t` epoch does not disappear, it just stops being anyone's problem
above the HAL. A new Core module, `kf/clock.h`, does integer-only civil-time
conversion (days-since-epoch to y/m/d and back, the standard integer algorithm
— no `float`, no libc `localtime`, so it is legal in `hakoniwaos/` and
identical on both targets). It is **stateless**: the RTC holds local time
directly, so there is nothing to store and nothing to apply — see "Timezone:
settled by Chris" at the end of this document. Sleep's night-window test and the
Lua binding call the *same* function, which is the real reason it belongs in
Core rather than in the binding: two implementations of "what hour is it
locally" is how the clock and the bedtime end up disagreeing.

**Editing with seven buttons and no keyboard.** A four-field cursor —
`HOUR → MINUTE → AM/PM → SAVE` — with:

- `LEFT` / `RIGHT` move between fields.
- `UP` / `DOWN` change the highlighted field. `AM/PM` toggles on either.
- `A` on `SAVE` commits; `A` on any other field advances (so a player who only
  ever presses `A` still gets through).
- `B` cancels and returns to Home without writing anything.
- `kf_app_buttons_held()` (`kf/app.h:135`) gives hold-to-repeat, so setting the
  minute does not mean 59 presses. Repeat starts after ~400 ms and fires at
  ~8 Hz. Both numbers are feel, not physics — Chris should try them and say.

The highlighted field is shown by inverting its text colours, which the scene
already supports (`kf_scene_set_colors(id, fg, bg)`), so no new drawing
primitive is needed. **No lowercase anywhere on this screen** — the font has
none, and a blank cell per letter is the silent failure the audience constraint
forbids.

### 3. Can LVGL actually be deleted once Info moves? — Yes, and the 256 KB is reclaimable one task earlier than the deletion.

What still reaches into LVGL today, traced file by file:

| Caller | What it needs | After Info moves |
|---|---|---|
| `kf_pet_info_screen.cpp` | the whole widget API | **deleted** |
| `kf_screen_nav.cpp` | `<lvgl.h>`, `lv_obj_t *root`, `lv_screen_load()`, `lv_obj_invalidate()` | loses all four; moves out of `simulator/src/lvgl/` |
| `sdl_main.cpp`, `app_main.cpp` | `kf_lvgl_port_init()`, `kf_lvgl_port_pump()`, guarded by `kf_screen_nav_wants_lvgl()` | the guard is always false; the pump and the init go |
| `sdl_debug_window.cpp:667-675` | reads `KF_ARENA_LVGL` for its arena HUD | one row disappears from the HUD |
| `lvgl_determinism_check` | `kf_lvgl_proof_screen.cpp` and golden `70c00d13cdfb6d97` | **tests LVGL itself.** Only meaningful if LVGL ships |
| `pet_screen_check` | `kf_pet_screen.cpp` (364 lines) and golden `132458f0171a2c0b` | **this is the only thing keeping that file alive** |
| `kf_lvgl_pool.cpp` ↔ `lvgl` | a deliberate CMake link cycle (`simulator/CMakeLists.txt:147`) | unwinds |
| `budget.h:195`, `:257`, `arena.h:56`, `arena.cpp:33` | the 256 KB arena and the total-budget assertion | the 256 KB is reclaimed |

**So: deleting LVGL requires retiring two tests and deleting a 364-line file
whose sole remaining purpose is one of them.** Say that plainly, because it is
the honest price. `pet_screen_check` is not a bad test — it is a golden
screenshot of a screen no user can reach, and once its subject is gone it is
asserting that a museum piece has not changed.

**But `CLAUDE.md` names "LVGL vs a custom sprite engine" as a deliberate later
evaluation, and deleting the dependency forecloses it.** So the plan does *not*
delete. Task 2 adds a CMake option `KF_ENABLE_LVGL`, **defaults it OFF**, and
puts the two LVGL-only tests and the 256 KB arena behind it. The default build
reclaims the memory and stops linking LVGL; `-DKF_ENABLE_LVGL=ON` still builds
and still passes 46/46 (44 plus the two LVGL-only checks), so the evaluation
stays available and the deletion
stays a one-line change Chris can make whenever he wants. That is the whole
prize (256 KB of PSRAM, plus LVGL's code out of a 672 KB firmware) without
spending the decision.

### 4. How does the clock get verified against a real power cut? — A bench procedure with a negative case, Task 5.

Not an assertion. A procedure, with Chris at the board, and it tests two things
that are easy to conflate: *did the RTC keep time*, and *did the firmware
believe it*.

The enabling change is a new observe-tier command, **`KFDBG RTC`**, that reads
the DS3231's registers over I2C **directly** and reports `epoch`, `osf` (the
oscillator-stopped flag) and `present`. Without it you cannot tell a working
coin cell from a RAM clock that happens to still be right, because
`KFDBG STATE` reports neither and `kf_time_wall()` returns the RAM clock on
both paths. The procedure and its pass criteria are Task 5's steps; the
negative case — **run it once with the coin cell removed** — is the half that
actually proves the code, because it is the only way to see `osf == 1`, the
wall clock correctly staying unset, and offline fast-forward correctly
*declining* to age the pet by a garbage interval.

### 5. What does the attention signal actually do? — A Core query, a game-side behaviour, and nothing that waits on hardware.

`kf_pet_wants(state, config)` returns one value from a small enum
(`NONE / FOOD / PLAY / REST / BATH / FLUSH / MEDICINE`), computed from the
thresholds the needs already cross, with a priority order and **hysteresis** so
it does not flap on and off at the boundary. It is a pure query over
`kf_pet_state` — no new save field, no new state to migrate — the same shape
`kf_pet_dominant_care_trait()` already has.

The behaviour is the demo script's, and it is three things stacked so it reads
at a glance across a room:

1. **The creature's own pose and position.** It stops wandering, moves to the
   front-centre of the field, and holds `objecting` — art that already exists
   for every stage in the pack.
2. **A pulsing indicator.** A single text object showing `!` in the reserved
   band, toggling visible/invisible at 1 Hz. One scene object, two setter calls
   per second, and the differ makes the still frames free. **`!` does not
   exist in the font yet** — `hakoniwaos/src/font_data.h`'s `0x21 '!'` entry is
   all zeroes (so is `?`, `0x3F`); the character set `tools/make_font.py`
   generates is deliberately limited to space, 0-9, A-Z and `. , : - / % + ( )`.
   Task 8 must add the `!` glyph to `GLYPHS` in `tools/make_font.py` and
   regenerate `font_data.h` as an explicit step (the module's own comment
   already calls new glyphs "a later, mechanical addition" — this is that).
   Do not ship the pulsing indicator against the current font: it would draw
   an invisible rectangle.
3. **The care-guide entry for the wanted action inverts** — the same
   `kf_scene_set_colors()` trick the Settings cursor uses. It tells the player
   *which* button, not merely *that* something is wrong.

**What defers to hardware, stated so nobody waits for it.** There is no
`kf/hal/audio.h` and no haptic HAL in this repo — the buzzer, the I2S speaker
and the vibration motor are on the target spec and are not built. The software
signal above must therefore be **complete on its own**, and it is: the pet is
legibly asking without making a sound. When audio arrives it hangs off the same
`KF_PET_WANT_NONE → something` transition this task creates, which is exactly
one call site. Do not design a sound API here.

---

## Global Constraints

Every task's requirements implicitly include this section.

- **`hakoniwaos/` stays heap-free.** `python3 tools/check_no_heap.py .` runs in
  `bash dev.sh test` and fails the build. `kf/clock.h`'s implementation and the
  sleep fields are plain integers in existing structures; nothing allocates.
- **`hakoniwaos/` stays free of floating point.** No `float`, no `double`. This
  bites hardest in Task 3: civil-time conversion has a well-known integer-only
  form, and the temptation to reach for `time.h`'s `localtime()` — which
  ESP-IDF does provide — must be resisted, because Core may only talk to the
  HAL. Every Lua numeric binding uses `luaL_checkinteger`, never
  `luaL_checknumber`.
- **240x320 RGB565**, colour-key transparency only (magenta
  `KF_RGB(255,0,255)`), sprites 48x48.
- **Maximum 8 dirty rectangles per frame** (`KF_MAX_DIRTY_RECTS`). Current
  worst case is 3. Staying under 8 is the engine's problem and must never
  become the script's.
- **The two golden rendering checksums must not move.**
  `headless_determinism` and `headless_fullscreen` run `main()`'s default path
  over Core's own demo and touch nothing this plan changes. If one moves,
  **stop** — something is wrong that has nothing to do with the intended change.
  Do not re-baseline them.
  `screen_nav_check`'s `ac44bb9819809bea` (`simulator/CMakeLists.txt:728`) is a
  different matter: Task 2 moves it legitimately, with before/after screenshots
  in that task's ADR. That is the only golden constant this plan may move, and
  only there.
- **Do NOT run `cmake -B build`.** Already configured; reconfiguring costs
  ~2 minutes. Build with `cmake --build build -j8`, test with
  `ctest --test-dir build`. **Desktop baseline is 44/44** and must stay 44/44
  plus whatever a task adds (minus what Task 2 deliberately gates off — that
  task states its own arithmetic).
- **ESP-IDF needs its environment sourced and this sandbox blocks bare
  `source`.** Write the sequence to a script and run it with `bash`:

  ```bash
  cat > /tmp/idf.sh <<'EOF'
  . $HOME/esp/esp-idf/export.sh > /dev/null 2>&1
  cd /Users/chris/Projects/kamiframe/.claude/worktrees/pixellab-mcp-server-960c5f/ports/esp32
  idf.py -DKF_PANEL=ili9341 build
  EOF
  bash /tmp/idf.sh
  ```

  `ili9341` is the panel connected to the board. Sourcing `export.sh` also puts
  pyserial 3.5 on the path, which system python3 lacks — `tools/kf_debug.py`
  needs it.
- **CI is the expensive place to fail, and it fails for things this machine
  cannot reproduce.** `.github/workflows/ci.yml`'s `linux-gcc` job builds with
  GCC and `-Werror`. It has already rejected `strncpy(dst, src, cap - 1)`
  followed by an explicit terminator under `-Wstringop-truncation`; the fix
  landed as commit `4bfd7f1` and **the codebase now uses
  `snprintf(dst, cap, "%s", src)` everywhere**. Neither that warning nor CI's
  older Python (which broke `read_text(newline=)`, commit `b90b535`) reproduces
  on the dev machine, so a CI-only failure costs a full round trip. Two rules
  for every task here: **copy the string-copy idiom from a neighbouring file
  rather than writing a new one**, and **stick to Python that a few releases
  back would accept**.
- **`kf/budget.h`'s banner forbids moving a budget number to make a test pass.**
  It does not forbid replacing an assumption with a measurement.

---

## Who has to be at the bench

| Task | Needs the board? | Needs Chris? |
|---|---|---|
| 1 — `kf.screen()` groups, the registry learns names | No | No |
| 2 — Info in Lua, LVGL gated off by default | No | Only to look at the before/after Info screenshots and agree they are the same screen |
| 3 — `kf/clock.h`, integer civil time in Core | No | No |
| 4 — the Lua time API and the Settings screen | No to build; **yes to judge** | Yes, to try the four-field editor and say whether the repeat rate feels right |
| 5 — `KFDBG RTC` and the power-cut test | **Yes** — board + DS3231 + CR2032, and a **physical unplug**, twice, once with the cell removed | Yes, unavoidably |
| 6 — sleep in Core | No | No |
| 7 — sleep on screen | No to build; **yes to judge** | Yes — bedtime is a feel question |
| 8 — the attention signal | No to build; **yes to judge** | Yes — how insistent it should be is his call |

Tasks 1, 2, 3 and 6 are entirely self-contained code changes that can be
written, reviewed and merged with the board in a drawer. **Do them first even
if it is on the desk.**

---

## Risks

| # | Risk | Retired by | Cost if it bites |
|---|---|---|---|
| 1 | **Two owners of "which screen is showing."** Lua's `screen:show()` and `kf_screen_nav.cpp`'s `load()` both switching is exactly the class of bug ADR 0042 documented and ADR 0043 fixed. | Task 1 makes `screen:show()` call *into* the registry, never around it, and its check switches screens from both sides and asserts one consistent result. | Stale pixels from the previous screen, appearing only on some transition orders. The worst kind to reproduce. |
| 2 | **The night window is computed against a clock Core does not advance.** `last_advanced` moves only at load (finding 2). | Task 6, step 1, before any sleep logic exists. | Sleep works after a reload and never during a session, or vice versa — and the symptom looks like a sleep bug, not a clock bug. |
| 3 | **Offline sleep needs analytic arithmetic, not a loop.** The spec says so: a fortnight offline cannot be stepped second by second, so the seconds falling inside a daily 22:00–07:00 window have to be solved as whole days plus two partials. | Task 3 builds and tests the window arithmetic **with no pet in the picture**, against hand-computed cases including DST-free month and year boundaries. Task 6 then only has to call it. | Silently corrupted offline ageing — the feature the entire product rests on, per the spec's own words. |
| 4 | **A power-cut test that passes for the wrong reason.** The RAM clock and the RTC agree in the good case, so a green result proves nothing unless the bad case was also run. | Task 5's negative run with the coin cell removed. | Shipping a device that forgets the time the first night a customer's cell is flat, having "verified" that it does not. |
| 5 | **The 64-object scene ceiling.** Three screens share one table. | **Retired by Task 4.** `run_settings_screen_check()` (`simulator/src/headless/headless_main.cpp`, `--verify-settings-screen`) brings up the real Home + Info + Settings together (not `run_screen_group_check()`'s synthetic two-screen fixture) and asserts `kf_scene_live_object_count()` against a named constant. **Measured, not the 47 estimated here**: Home is 25 (24 declared in `creature.lua` + 1 error banner, unchanged), Info is 8 (unchanged), Settings is 10 (9 declared — title, 3 captions, 3 editable values, a SAVE row, a BACK hint — + its own error banner, well under the 14-object budget Task 4's brief allotted). **43 of 64**, with more headroom than this table assumed. | Retired: the check above fails loudly and by name (not just "the scene is full") if this ever regresses. |
| 6 | **A Lua script can still hang the frame loop.** No `lua_sethook`, no deadline. Named in ADR 0014 and ADR 0028; Task 9 of the Lua plan. | **Nothing in this plan.** | A frozen device needing a power cycle. Acceptable while Chris is the only author; not acceptable before third parties ship. It gets worse with every screen Lua owns, and this plan hands it two more. |

---

## What a first session should land

**Tasks 1, 2 and 3 — with Task 3 as the stretch.**

- **Task 1** changes no pixels. It is the enabling mechanism: `kf.screen()`
  groups and a registry that learns names instead of hardcoding two. Home
  becomes screen "home", declared exactly as it is today, and every existing
  check must pass unchanged including `screen_nav_check`'s golden constant.
- **Task 2** is the one the owner asked for first and the one he can *see*:
  Info rendered by the retained scene instead of by LVGL, and LVGL switched off
  by default, reclaiming 256 KB. It moves one golden constant, deliberately,
  with screenshots.
- **Task 3** adds a Core module nothing calls yet. It is cheap, it is pure
  integer arithmetic with hand-checkable answers, and it is the piece that
  Tasks 4 *and* 6 both block on — so getting it in early is worth more than its
  size suggests. If the session is running long, **split Task 2's LVGL gating
  into its own commit and stop there**; Info-on-the-scene is complete and
  shippable without it.

After a first session: three screens work, the Info screen is Lua's, LVGL is
optional, and Core can answer "what hour is it locally" correctly. Nothing
about the clock or sleep is visible to a player yet.

**Not a first session: Tasks 4–8.** Task 4 is a whole screen with an
interaction model. Task 5 needs Chris, a coin cell and two unplugs. Task 6 is
the save-format bump and the offline arithmetic and deserves a fresh session
with nothing else in it. **This is four features and it is roughly four
sessions of work, not one.** Sequencing them into one night is how the offline
fast-forward gets quietly corrupted.

---

## Task sequence

Tasks 1–3 are specified to step level. Tasks 4–8 are specified to requirement
level, because their detail depends on what Tasks 1–3 measure and writing it
now would be inventing.

---

### Task 1: `kf.screen()` — named object groups, and a registry that learns names

**Why first:** it changes no pixels, it is the smallest thing that makes three
screens possible, and every task after it depends on the answer. Doing it
before Info means Info is written against the mechanism rather than the
mechanism being retrofitted around Info.

**Files:**
- Modify: `sdk/lua/kf_lua_scene.cpp`, `sdk/lua/kf_lua_scene.h` (the screen
  group type and its metatable)
- Modify: `simulator/src/lvgl/kf_screen_nav.cpp`, `kf_screen_nav.h`
  (register-by-name; fix the stale header at `:4-5`)
- Modify: `simulator/src/sdl/sdl_debug_window.cpp` (delete the local
  `screen_name()` switch; query the registry)
- Modify: `examples/creature_demo/creature.lua` (Home's objects are declared
  through a screen group)
- Test: `simulator/src/headless/headless_main.cpp` — `run_screen_group_check()`,
  flag `--verify-screen-groups`
- Modify: `simulator/CMakeLists.txt` (register `screen_group_check`)
- Create: `docs/architecture/adr-0044-lua-screen-groups.md`

**Interfaces:**
- Consumes: `kf_scene_add_*()`, `kf_scene_set_visible()`,
  `kf_scene_force_repaint()` (`kf/scene.h`); `kf_app_buttons_pressed()`
  (`kf/app.h:136`).
- Produces, in `kf_screen_nav.h`:
  `int kf_screen_nav_register(const char *name, void (*update)(uint32_t dt_ms))`
  returning the new index; `const char *kf_screen_nav_name(int index)`;
  `int kf_screen_nav_count(void)`; `void kf_screen_nav_show(int index)`. The
  three existing `kf_screen_nav_debug_*` entry points keep their exact
  signatures — `sdl_debug_window.cpp:444` and `:870` call them and Task 1 must
  not touch those call sites except to delete the local name switch.
- Produces, in Lua: `kf.screen(name)` returning a screen handle, with
  `screen:sprite(name)`, `screen:text(str)`, `screen:box(w, h, color)`,
  `screen:background(color)`, `screen:show()`, and `screen:name()`.

**Design note — the trap is two owners of the current screen.**

`screen:show()` must not hide and unhide objects itself and *also* let
`kf_screen_nav_frame()`'s MENU handling do the same thing by another route. One
of them wins on some transition orders and the other wins on the rest, and the
symptom is Info's pixels surviving under Home — which is precisely the bug ADR
0042 recorded as a known gap and ADR 0043 closed with
`kf_scene_force_repaint()`. Reintroducing it through a second switching path
would be a genuine regression against a fix that cost a task.

So: **`screen:show()` calls `kf_screen_nav_show(index)` and does nothing else.**
The registry, and only the registry, does the work — hide every other screen's
objects, show this one's, apply this screen's background colour, then
`kf_scene_force_repaint()`. Both the MENU edge and the Lua call arrive at the
same function.

Second trap: **`kf.sprite()` and friends must keep working unchanged.** Nine
existing bindings and `examples/hello_pet/pet.lua` use them, and
`kf_error_banner.cpp` calls `kf_scene_add_text()` from C++ with no screen at
all. Define it once and comment it at the binding: an object created outside
any group belongs to no screen and is **never** touched by `show()` — which is
exactly the behaviour the error banner needs, since a banner that vanished when
you changed screens would hide the error that caused it.

Third trap: the background. `kf_scene_set_background_color()` is scene-wide,
singular. Each screen therefore stores its own colour and `show()` applies it.
A screen that never called `screen:background()` inherits whatever was last set
— document that rather than inventing a default, because a silently-black
Settings screen and a silently-inherited one are both defensible and only one
of them is what the author meant.

- [ ] **Step 1: Write the failing check**

Add `run_screen_group_check()` to `simulator/src/headless/headless_main.cpp`,
near the other `run_*_check()` functions, registered as
`--verify-screen-groups`. Match the neighbours' reporting style; do not
introduce a third. It must:

1. Load a small inline Lua script declaring **two** screens, each with a
   distinct background colour and one text object at a known position.
2. Show screen A, commit, hash the framebuffer.
3. Show screen B, commit, assert the framebuffer differs and that **none of
   screen A's pixels survive** — compare a sample inside A's text bounds
   against B's background colour.
4. Show screen A again, commit, and assert the hash equals step 2's. This is
   the ADR 0043 re-entry property, now under a second screen's worth of load.
5. Drive the same two transitions through `kf_screen_nav_debug_advance()`
   instead of `screen:show()` and assert identical hashes. **This is the
   anti-two-owners assertion** and it is the point of the whole check.
6. Assert the live scene-object count after all screens are declared, against
   a named constant, so risk 5 fails with a number.

- [ ] **Step 2: Run it and watch it fail for the right reason**

```
cmake --build build -j8
./build/kamiframe-headless --verify-screen-groups
```

Expected: a Lua error that `kf.screen` is nil. If it fails on anything else —
a link error, an assertion about the scene — fix that first.

- [ ] **Step 3: The screen group in the Lua binding**

In `sdk/lua/kf_lua_scene.cpp`, add a screen userdata with its own metatable,
alongside the existing object metatable. A screen holds its name, its
background colour, its registry index, and the list of `kf_scene_id`s created
through it. **A fixed-capacity array, not a Lua table** — the same reasoning
`kf_lua_scene.cpp` already applies to objects, and the cap is
`KF_SCENE_MAX_OBJECTS` so it can never be the binding that overflows first.

`kf.screen(name)` creates-or-fetches by name, so calling it twice in one script
is not an error and does not make a second screen. Registering with
`kf_screen_nav_register()` happens on creation.

- [ ] **Step 4: The registry stops being two fixed entries**

In `kf_screen_nav.cpp`, replace `constexpr size_t kScreenCount = 2` and the
two hardcoded `g_screens[]` assignments with a fixed-capacity table (**8 is
plenty** — this is a handheld, not a desktop) filled by
`kf_screen_nav_register()`. Home still registers first and is still index 0, so
`B`-jumps-home and `kf_screen_nav_debug_home()` keep meaning what they mean.
Registering past the cap logs once naming the limit and returns -1.

Keep `kf_screen_nav_wants_lvgl()` exactly as it is. Info is still LVGL until
Task 2 and this task must not touch that.

Fix `kf_screen_nav.h:4-5` while you are in the file: it still says Home is
`kf_pet_screen.cpp`, which stopped being true two tasks ago and is contradicted
by the same header at `:37-47`.

- [ ] **Step 5: The debug window asks instead of remembering**

Delete `screen_name()` from `sdl_debug_window.cpp` (`:216-224`) and read
`kf_screen_nav_name()` at `:870` instead. Keep a `"?"` for an out-of-range
index — a debug readout should never crash — but it stops being reachable for
a screen that simply was not added to a switch.

- [ ] **Step 6: `creature.lua` declares Home through a group**

The whole `if kf.home_screen_active() then ... end` block becomes
`local home = kf.screen("home")` and `home:sprite(...)` / `home:box(...)` /
`home:text(...)` in place of the bare `kf.` calls. **Nothing about the layout,
the order of declaration, or the object count changes** — this is a receiver
change and nothing else, which is what keeps `screen_parity_check` and
`screen_nav_check` green.

Keep the comment additions to one line. Script comments cost flash.

- [ ] **Step 7: Full suite, then cross-compile**

```
cmake --build build -j8
./build/kamiframe-headless --verify-screen-groups
ctest --test-dir build
```

Expected: **45/45**. `screen_nav_check` must still produce `ac44bb9819809bea`
— this task changes no pixels, so if that constant moves, the receiver change
was not as neutral as it looks and something in declaration order shifted.
`screen_parity_check` must still pass, which is the stronger statement of the
same thing.

```bash
bash /tmp/idf.sh
```
Expected: clean, zero warnings, firmware within a few hundred bytes of
~672 KB. Record the figure.

- [ ] **Step 8: ADR and commit**

`docs/architecture/adr-0044-lua-screen-groups.md`: groups over one scene rather
than one scene per screen, and the 47-of-64 arithmetic that makes it fit; the
single-owner rule for switching and the ADR 0042/0043 bug it exists to prevent;
ungrouped objects and why the error banner must be one; per-screen backgrounds;
why the registry stayed in C++ (the three debug-window couplings, named); a
"Not verified" section stating that no Lua screen group has rendered on
hardware.

**How you would know it worked:** add a third screen to a scratch script,
switch to it with the SDL debug window's "Next Screen" button, and see its name
in the readout — not `"?"`, and without editing a switch statement. If that
loop needs a C++ edit, the task is not done.

---

### Task 2: Info leaves LVGL, and LVGL leaves the default build

The owner's number one. The Info screen becomes 8 scene objects declared in
Lua, `kf_pet_info_screen.cpp` is deleted, and LVGL goes behind an option that
defaults off.

**Files:**
- Modify: `examples/creature_demo/creature.lua` (declare the Info screen)
- Modify: `sdk/lua/kf_lua_port.cpp` (the `pet.*` reads Info needs, if any are
  missing — check first, most exist)
- Delete: `simulator/src/lvgl/kf_pet_info_screen.cpp`, `kf_pet_info_screen.h`
- Move: `simulator/src/lvgl/kf_screen_nav.{cpp,h}` →
  `simulator/src/pet/kf_screen_nav.{cpp,h}`, dropping `<lvgl.h>`
- Modify: `simulator/CMakeLists.txt`, `ports/esp32/main/CMakeLists.txt`
  (`KF_ENABLE_LVGL`, default `OFF`; the two LVGL-only tests gated behind it)
- Modify: `simulator/src/sdl/sdl_main.cpp`, `ports/esp32/main/app_main.cpp`
  (the LVGL init and pump become conditional)
- Modify: `hakoniwaos/include/kf/budget.h`, `kf/arena.h`, `hakoniwaos/src/arena.cpp`
  (the 256 KB arena is only carved when LVGL is enabled)
- Modify: `simulator/src/sdl/sdl_debug_window.cpp:667-675` (the arena HUD row)
- Create: `docs/architecture/adr-0045-info-screen-in-lua.md`

**Requirements:**

- [ ] Info declares **eight** text objects matching what
      `kf_pet_info_screen.cpp` shows today: title, `STAGE` caption and value,
      `TIME IN STAGE` caption and value, the branch line, `PERSONALITY` caption
      and the trait line. Everything it needs already exists in the `pet.*`
      binding (`stage`, `teen_form`, `adult_branch`, `base_trait`,
      `dominant_care_trait`) — **check before adding anything**; the one gap is
      likely `stage_elapsed_seconds`, and if so add `pet.stage_seconds()` as a
      plain integer getter.
- [ ] The duration formatter — `"2D 4H"`, `"3H 12M"`, `"5M 09S"`, `"42S"` —
      moves into the Lua script, **uppercase**, because the bitmap font has no
      lowercase and LVGL's did. This is a real visible difference from today's
      screen and it is the expected one; it is why the golden constant moves.
- [ ] The blank-until-meaningful rules survive the move exactly: the branch
      line is empty before TEEN, and the care trait is omitted while still an
      egg. `kf_pet_info_screen.cpp:118-131` documents why. **Copy the reasoning
      into the ADR, not into the script** — script comments cost flash.
- [ ] `screen_nav_check`'s golden constant `ac44bb9819809bea`
      (`simulator/CMakeLists.txt:728`) **moves here, legitimately and only
      here.** Capture a PNG of Info before and after and put both in the ADR.
      This is the only circumstance in which this project re-baselines a golden
      constant, and the evidence is the price of it.
- [ ] `kf_screen_nav.cpp` moves to `simulator/src/pet/` and loses `<lvgl.h>`,
      `lv_obj_t *root` from `ScreenEntry`, `lv_screen_load()`,
      `lv_obj_invalidate()`, and `kf_screen_nav_wants_lvgl()`. Its long
      `load()` comment about LVGL's dirty tracking goes with them — **delete
      it, do not leave it describing machinery that is gone.** Fix
      `kf_pet_info_screen.h`'s stale references by deleting the file.
- [ ] `KF_ENABLE_LVGL` is a CMake option on both build systems, default `OFF`,
      validated at configure time. With it OFF: `kamiframe_lvgl_port` is not
      built, `lvgl_determinism_check` and `pet_screen_check` are not
      registered, `KF_ARENA_LVGL` is not carved, and `budget.h:257`'s
      total-arena assertion is recomputed without it. With it ON: everything
      builds and **46/46 passes** (the default build's 44 plus the two
      LVGL-only checks). State both counts in the task's report — the
      default build's number goes *down* by two relative to ON, and that is
      correct, not a regression.
- [ ] `kf_pet_screen.cpp` is **not deleted.** It survives as
      `pet_screen_check`'s subject under `-DKF_ENABLE_LVGL=ON`. Deleting it and
      retiring that test is a real, defensible follow-up and it is **Chris's
      call**, not this task's — `CLAUDE.md` names LVGL a deliberate later
      evaluation. Say so in the ADR and leave it.
- [ ] Measure and record: firmware size with LVGL off versus on, and the PSRAM
      figure from the boot log (`heap_free_psram` in `KFDBG STATE`) with each.
      **The 256 KB is the headline result of this task** and it should be
      stated as a measurement, not a subtraction.
- [ ] Cross-compile both ways with `-DKF_PANEL=ili9341`. Zero warnings.

**How you would know it worked:** press MENU on the board and see the Info
screen, drawn by the same blitter that draws the creature, with LVGL not
linked into the image at all. And `heap_free_psram` up by roughly 262,144.

---

### Task 3: `kf/clock.h` — civil time in Core, integers only

No UI, no Lua, nothing calls it. The piece Tasks 4 and 6 both block on, built
and tested on its own, which is the only way the night-window arithmetic gets
the "got right the first time" the spec demands of it.

**Files:**
- Create: `hakoniwaos/include/kf/clock.h`, `hakoniwaos/src/clock.cpp`
- Modify: `hakoniwaos/sources.cmake` — **one line, one place.** That file is
  the single source list both builds read, and its own header explains it is
  the mechanism stopping the two from drifting. Do not touch either
  `CMakeLists.txt` directly.
- Test: `simulator/src/headless/headless_main.cpp` — `run_clock_check()`, flag
  `--verify-clock`
- Modify: `simulator/CMakeLists.txt` (register `clock_check`)
- Create: `docs/architecture/adr-0046-core-civil-clock.md`

**The API this task produces.** Names Tasks 4 and 6 both depend on. Updated
2026-08-13 after "Timezone: settled by Chris" below landed mid-task-3: **no
UTC offset field.** The RTC holds local time directly — the wall clock's own
epoch counter already means "local", because the owner dials it in by hand.
`kf/clock.h` therefore does not carry, set, or apply any offset; it converts
between that local epoch and a calendar date, and nothing else:

```c
typedef struct {
    int32_t year;    /* e.g. 2026 */
    uint8_t month;   /* 1..12 */
    uint8_t day;     /* 1..31 */
    uint8_t hour;    /* 0..23 */
    uint8_t minute;  /* 0..59 */
    uint8_t second;  /* 0..59 */
} kf_civil;

void    kf_civil_from_epoch(int64_t epoch_seconds, kf_civil *out);
int64_t kf_epoch_from_civil(const kf_civil *in);

/* Seconds of [from, to) that fall inside the daily [start_hour, end_hour)
 * window, where a window whose end_hour is <= start_hour wraps midnight.
 * Solved analytically -- whole days plus two partials -- never by stepping.
 * Task 6's night accounting is exactly this call. */
int64_t kf_clock_seconds_in_daily_window(int64_t from, int64_t to,
                                          uint8_t start_hour, uint8_t end_hour);
```

**Design note — the trap is reaching for `localtime()`.**

ESP-IDF ships one and so does the host, so it will compile on both targets and
look correct. It is still wrong here for three reasons that all bite later:
Core's rule is that it talks to the HAL and nothing else; `localtime()` depends
on `TZ`, which is process state neither target sets and which would make the
same epoch produce different answers on desktop and device; and it drags in
locale machinery on a build that counts kilobytes. The integer form — days
since the epoch to a civil date and back — is about twenty lines, is
well-documented, and has no state of its own at all (no offset, no anything —
this module is stateless, which is a direct consequence of the no-offset
decision, not an unrelated simplification).

Second trap, and it is the one that will actually cost time: **the window
function must be right at the edges, and the edges are where a loop-based
implementation and an analytic one disagree.** A `from` that is already inside
the window; a `to` inside the same window on the same day; a span shorter than
the window; a span of exactly one day; a span crossing a month end, a year end,
and a leap day. And floor division on a negative numerator still matters even
with no offset to go negative: any window whose `start_hour` shift pushes an
early instant below zero, or any epoch before 1970 (a bogus RTC value, or a
test), truncates toward zero rather than flooring in plain C division, which
is the classic off-by-one-day here.

Third: no DST, and no timezone conversion of any kind — settled by Chris, see
"Timezone: settled by Chris" below. State it in the header as a deliberate
limitation rather than letting someone discover it: the RTC's local time is
"what the person holding it told us", set by hand on the Settings screen
(Task 4) today, and an internet time sync is real future work the board's
WiFi makes possible, not a hypothetical this module needs to design around now
beyond keeping its own conversion total and honest.

- [ ] **Step 1: Write the failing check**

`run_clock_check()`, `--verify-clock`. It must, in this order:

1. Round-trip: for a spread of known epochs — a leap day, a year boundary, an
   hour before and after midnight, epoch 0 — assert
   `kf_epoch_from_civil(kf_civil_from_epoch(e)) == e`. No offset variants:
   there is no offset parameter any more, so one pass over the spread is the
   whole test.
2. Assert specific hand-computed civil values for at least three epochs. Write
   the expected values into the check from an independent calculation (Python's
   `datetime.utcfromtimestamp` is fine as the oracle **while writing the
   check**; the assertion in the source is a literal, and the ADR records where
   it came from).
3. Window arithmetic for a 22→07 wrap, against hand-computed answers: a span
   entirely inside the night; entirely outside; starting mid-night; ending
   mid-night; exactly 24 hours from an arbitrary instant (**must be exactly
   9 * 3600**, whatever the start time — that single assertion catches most
   partial-day bugs on its own); 14 days; and 14 days crossing a month
   boundary (replaces the old "-5h offset" case, which no longer applies now
   that there is no offset — a month-boundary span is the better use of that
   test slot, since it exercises `kf_epoch_from_civil`'s day-arithmetic across
   a calendar edge the plain 14-day case does not touch).
4. Anti-vacuity: the check must **fail** if
   `kf_clock_seconds_in_daily_window()`'s body is replaced with `return 0`.
   Verify by actually doing it once and watching it go red.

- [ ] **Step 2: Run it, watch it fail on the link**

```
cmake --build build -j8
./build/kamiframe-headless --verify-clock
```

Expected: an undefined-symbol link error. Anything else, fix that first.

- [ ] **Step 3: Implement, with no heap and no float**

`hakoniwaos/src/clock.cpp`. No file-static state at all — with no offset to
hold, the module is pure functions over their arguments.
`python3 tools/check_no_heap.py .` must stay clean, and there must be no
`float` or `double` anywhere in the file.

- [ ] **Step 4: Suite and cross-compile**

```
cmake --build build -j8
./build/kamiframe-headless --verify-clock
ctest --test-dir build
python3 tools/check_no_heap.py .
bash /tmp/idf.sh
```

Expected: the new check passes; the suite is Task 2's count plus one; heap
check clean; ESP-IDF clean with zero warnings.

- [ ] **Step 5: ADR and commit**

`docs/architecture/adr-0046-core-civil-clock.md`: why Core owns civil time
rather than the Lua binding (sleep and the clock display must agree, and one
implementation is how they agree); why not `localtime()`; the integer algorithm
and where it came from; the analytic window rule and the whole-days-plus-two-
partials shape; no DST and no offset at all — the RTC holds local time
directly, set by hand on the Settings screen (Task 4), per "Timezone: settled
by Chris" — and why that keeps this module's conversion honest for a future
internet time sync (it only has to set the clock, never reinterpret a stored
timestamp); a "Not verified" section stating nothing calls this yet.

**How you would know it worked:** `--verify-clock` passes and fails when the
window function is stubbed to zero. There is nothing visible to see; that is
the point of building it here rather than inside the settings screen.

---

### Task 4: The time API in Lua, and the Settings screen

First user-visible clock. Desktop first; Chris judges the feel before it goes
near the board.

**Requirements:**

- [ ] `kf.time()`, `kf.hour()`, `kf.minute()`, `kf.clock_set()` and
      `kf.set_clock(hour, minute)`, as specified in the answer to question 2
      above. Every numeric argument through `luaL_checkinteger`. `kf.time()`
      returns a **12-hour string with `AM`/`PM`, uppercase**, e.g. `"9:05 AM"`
      — every character in it is in the font's set (`kf/font.h`: digits, `:`,
      and uppercase letters). `kf.time()` on a device whose clock has never
      been set returns `"--:-- --"`, not a lie and not an empty string.
- [ ] `kf.set_clock()` preserves today's date and the seconds field and
      calls `kf_time_set_wall()` directly — the epoch it writes IS local time,
      so there is no offset to apply. It returns `false`
      rather than raising when the backend refuses (`KF_ERR_UNAVAILABLE` on a
      read-only clock is a documented HAL return), so a script can say so on
      screen.
- [ ] **Nothing about the clock persists outside the RTC itself.** There is no
      UTC offset to store, because the RTC holds local time directly. Resist
      adding a storage key "ready for" the internet-sync feature: a field
      nothing sets is wrong the first time something reads it, and once it is in
      a save format it has to be carried forever. When sync lands it sets the
      clock; it does not reinterpret stored timestamps.
- [ ] The Settings screen is a Lua screen group (Task 1), declared in
      `examples/creature_demo/creature.lua`, registered third so MENU cycles
      `HOME → INFO → SETTINGS → HOME`. **Budget: 14 objects** — title, the four
      editable fields, four field captions, a `SAVE` row, a `BACK` hint, and
      three spare. If it needs more, count the whole scene before adding them
      (risk 5).
- [ ] **A colour background, never a sprite** — finding 4. A sprite background
      makes every clock tick a full-screen repaint.
- [ ] The four-field cursor and its button map, exactly as specified in the
      answer to question 2. Hold-to-repeat via `kf_app_buttons_held()`.
- [ ] **The clock updates by setting the same string every frame** —
      `clock:set(kf.time())` — and the differ makes the 59 frames a second
      where it has not changed cost nothing. **Confirm** that
      `kf_scene_commit()` genuinely compares the text content and not just the
      fact that a setter was called; if it does not, that is a differ bug worth
      more than this task and it should be reported rather than worked around.
- [ ] `MENU` is consumed by the navigation registry, so a script **must not**
      bind it with `kf.on_button("menu", ...)`. State that in the SDK style
      guide and in the ADR. It is the one button that is not the game's, and
      discovering that by having your handler fire on every screen change is a
      bad first experience.
- [ ] A headless check drives the editor: enter Settings, move the cursor
      through all four fields, change the hour, save, and assert
      `kf_time_wall()` moved by the expected number of seconds and by nothing
      else. Then cancel with `B` from a modified state and assert the clock did
      **not** move — the cancel path is the one nobody tests and the one that
      silently corrupts a clock.
- [ ] The minimal listing below is the acceptance test for the API surface. If
      showing a clock takes more than this, the API is wrong and should change
      before this task is dispatched:

      ```lua
      local settings = kf.screen("settings")
      settings:background(kf.color(20, 24, 32))

      local clock = settings:text("")
      clock:move(84, 140)
      clock:color(kf.WHITE, kf.color(20, 24, 32))

      -- Inside on_frame: free on every frame the string has not changed.
      clock:set(kf.time())
      ```

- [ ] Cross-compile. Then flash and let Chris set the time with the physical
      buttons and say whether the repeat rate and the field order feel right.
      **That judgement is the task's real acceptance**, not the check.
- [ ] ADR 0047: the four-function surface and why no epoch reaches Lua; the
      button map; that the clock persists nowhere but the RTC and why (no
      storage key, "ready for" internet sync or otherwise) and the
      device-wide decision behind it; the MENU reservation; a "Not verified"
      section — the DS3231 write path is still unproved on silicon, which is
      Task 5.

---

### Task 5: `KFDBG RTC`, and the power-cut test

> ## STATUS 2026-08-11: the core risk is RETIRED. Three sub-requirements are not.
>
> **What was actually run**, by Chris at the bench, board powered from USB with
> the DS3231 coin cell fitted. The clock was set from the Lua Settings screen,
> then read from the boot log line `DS3231: wall clock set from RTC
> (epoch ...)` across three boots, with USB **fully removed** for about a
> minute between the second and third:
>
> | Reading | Epoch | Delta |
> |---|---|---|
> | first boot | 1786384202 | — |
> | before the power cut | 1786384432 | +230 s |
> | after ~1 min unplugged | 1786384549 | **+117 s** |
>
> **+117 s across a genuine power cut, OSF clear on all three boots.** The
> cell holds time; the write-through reaches the chip from a production
> caller; the register map and the temperature-based DS3231/MPU-6050
> disambiguation are correct on real silicon. Recorded in
> `docs/architecture/adr-0026-ds3231-rtc-driver.md` under "Confirmed on
> hardware, 2026-08-11", and that ADR's two stale "not reached" bullets and
> `ports/esp32/hal/esp_time.cpp`'s "NOT yet run against real hardware" header
> have been corrected. **ADR 0048 was not written** — the result went into
> ADR 0026, where the claims it falsifies actually lived.
>
> **Do not treat this task as closed.** Three requirements below were NOT
> done, and the first two of them are the ones that would catch a fault:
>
> 1. **`KFDBG RTC` was never built.** The measurement above used the boot log
>    instead, which is minute-resolution-adequate for a power-cut test but
>    reads the RAM clock's *source* only at boot and cannot compare RAM
>    against chip at will. The command is still worth having; it is now
>    unblocked and independent of Tasks 6–8.
> 2. **The negative case — coin cell removed — was NOT run.** So the OSF-set
>    branch, the "wall clock not set yet" skip in
>    `hakoniwaos/src/pet.cpp:1201`, and "the pet does not age on a dead cell"
>    all remain unexercised on hardware. The plan called this "not optional"
>    and it was right to.
> 3. **Offline *ageing* was NOT confirmed on device.** `stage_elapsed_s` was
>    never captured either side of the cut. We proved the *clock* survives;
>    we did not observe the *pet* consuming that elapsed time on hardware.
>    The simulator covers this path thoroughly, and the RTC half is now
>    proven, so this is a gap in observation rather than a suspected fault —
>    but it is the actual product, so say "unobserved", not "working".
>
> One thing the run surfaced that the plan did not anticipate: the board's
> **date** was a day behind while its time-of-day was correct, because
> `kf_lua_port_apply_clock()` preserves the RTC's existing date and the
> Settings screen edits hour and minute only. Nothing reads the date today,
> so it breaks nothing — but there is no in-app way to fix a drifted date.
> Noted in ADR 0026 and in `ports/esp32/README.md`'s open-questions list.

**Needs Chris, the board, a DS3231, a coin cell, and two physical unplugs.**
Retires the risk that `esp_time.cpp:311`'s write-through has never executed on
silicon and that nobody knows whether the cell holds time.

**Requirements:**

- [ ] `KFDBG RTC` — observe tier, gated by `KF_DBG_BRIDGE_ENABLE` only, never
      by `KF_DBG_MUTATE_ENABLE`, because it changes nothing. It reads the
      DS3231's registers over I2C **directly**, not `kf_time_wall()`, and
      replies with `present`, `epoch`, `osf`, and the RAM clock's `wall` and
      `wall_valid` alongside so the two can be compared in one line. Reading
      the RAM clock instead would make the whole test vacuous.
- [ ] **Check the reply buffer arithmetic.** `handle_state()`'s buffer is sized
      by an explicit computation documented at `kf_dbg_bridge.cpp:228`. If this
      task adds keys to `STATE` as well as adding a command, redo that
      computation and say in the comment what the new figure was computed
      from — a truncated reply surfaces to the host as a JSON parse error miles
      from the cause.
- [ ] Host side: `python3 tools/kf_debug.py rtc`, plus `tools/kf_debug_selftest.py`
      coverage for a matching reply, an `osf == 1` reply, and an `err` when no
      chip answered. Wire protocol changes land in `kf_dbg_bridge.cpp`,
      `kf_debug.py` and `kf_debug_selftest.py` **in the same commit** — one
      contract in two languages.
- [ ] **The procedure, run and recorded.** Chris at the bench:
      1. Flash with `-DKF_PANEL=ili9341`, coin cell installed.
      2. Set the time on the Settings screen with the buttons. Note it, and the
         real wall-clock time from a phone.
      3. `python3 tools/kf_debug.py rtc` — `present` true, `epoch` matching
         within a second or two, `osf` 0. **If `osf` is 1 here the write-through
         did not clear it and Task 4's setter is wrong.**
      4. Note `stage_elapsed_s` from `KFDBG STATE`.
      5. **Unplug USB. Fully off.** Wait a measured interval — ten minutes
         minimum to prove the mechanism, overnight to prove the cell.
      6. Plug back in. Touch nothing. `rtc` then `state`.
      7. **Pass:** `epoch` advanced by the real elapsed interval within a couple
         of seconds, `osf` still 0, and `stage_elapsed_s` advanced by the
         offline interval — the pet aged while it was off, which is the product.
- [ ] **Then the negative case, and it is not optional.** Repeat with the coin
      cell **removed**. Expected: `present` true (the chip is powered from
      3V3), `osf` **1**, the RAM clock unset, `kf_pet_load_and_advance()`
      logging `wall clock not set yet -- skipping offline fast-forward`
      (`hakoniwaos/src/pet.cpp:1201`), and the pet's age **unchanged**. A green
      first run without this one proves only that a powered chip keeps time,
      which was never in doubt.
- [ ] ADR 0048 records the actual numbers seen in both runs, and updates
      `ports/esp32/hal/esp_time.cpp`'s header — which currently says **"NOT yet
      run against real hardware"** and, after this task, will be either wrong or
      the most important sentence in the file. Also update
      `docs/architecture/adr-0026-ds3231-rtc-driver.md`'s "Not verified"
      section, which this task is finally in a position to close.

---

### Task 6: Sleep in Core

> ## STATUS 2026-08-11: LANDED. 47/47, `hokorimaru_check` unmodified, ESP-IDF
> ## clean. See ADR 0048 for the full design record and the non-vacuity proof
> ## for every new assertion.
>
> All ten requirements below landed as specified — no requirement in this
> section turned out to be wrong against the code, unlike some earlier tasks
> in this plan. Two things worth recording that the requirement list itself
> did not spell out, both in ADR 0048 at length:
>
> - **"Whatever the drowsy state needs" turned out to be nothing beyond the
>   one boolean.** There is no separate persisted drowsy sub-state in Core;
>   settling into bed is entirely the game layer's decoration (Task 7),
>   never read by anything this task built.
> - **The neglect-pause mechanism is a small, local change, not a rewrite**
>   of the existing three-way neglect-crossing logic: every one of its three
>   cases already produces a neglected range that is a SUFFIX of the segment,
>   so the range's own start offset was already sitting in the code as
>   `cared_for`, reused directly rather than recomputed.
>
> Two real bugs were found and fixed while writing this task's own tests
> (both in the tests, not in `pet.cpp`/`creature.cpp`) — see ADR 0048's "A
> real bug found while writing this task's own tests" for both. Neither
> reached the committed suite.

The spec is settled and complete; this is implementation. **A fresh session,
with nothing else in it** — it changes the save format and the offline
fast-forward, which is the feature the whole product rests on.

**Requirements, in dependency order:**

- [x] **First, before any sleep logic: `kf_pet_advance()` carries
      `last_advanced` forward** by the same elapsed seconds that drive decay.
      Today it moves only in `kf_pet_load_and_advance()` (`pet.cpp:1229`), so
      during live play Core does not know the time and the night window cannot
      be evaluated. The care-loop spec prescribes exactly this and calls it *"a
      good change regardless of what sleep ends up looking like"*. Land it with
      its own check — that the pet's notion of now tracks a session's elapsed
      time — **before** anything reads it. **Landed**: `kf_pet_advance()` now
      threads a `have_clock`/`cursor` pair through its stage loop, updating
      `state->last_advanced.epoch_seconds` at every segment. Its own dedicated
      check is case 6 of `run_pet_check()` (`--verify-pet`), green before any
      sleep logic existed.
- [x] `bool asleep` and whatever the drowsy state needs join `kf_pet_state`.
      `kSaveVersion` 8→9, `KF_PET_SAVE_BYTES` grows, `pack()`/`unpack()` and
      the `KF_ASSERT(off == KF_PET_SAVE_BYTES)` at `pet.cpp:717` all move
      together. A version-8 save is **refused**, matching the existing policy at
      `:739` — this project does not guess at a layout that changed. **Landed**:
      `KF_PET_SAVE_BYTES` is now 92 (91→92, one byte); "whatever the drowsy
      state needs" turned out to be nothing else, per the STATUS block above.
- [x] Night is **22:00–07:00 local**, via `kf_clock_seconds_in_daily_window()`
      from Task 3. Core does not re-derive it. **Landed** — two private
      constants in `pet.cpp` (`kNightStartHour`/`kNightEndHour`), every
      window question routed through that one function, including the
      point-in-time "is it asleep now" query.
- [x] **Falling asleep is automatic** and the live and offline rules are the
      same rule — which is what makes offline tractable. Waking is entirely the
      creature's own, so "asleep forever" is unreachable. **Landed** — one
      code path (`apply_stage_segment()`), no online/offline branch anywhere
      in it.
- [x] **`neglect_seconds` does not advance while asleep; the needs keep
      decaying.** Two different things, and the distinction is the point.
      **Landed** as an overlap subtraction against the existing neglected
      range, not a rewrite — see the STATUS block above and ADR 0048 section
      5.
- [x] **The compression goes in the thresholds, not the accrual.** Cut
      `sickness_onset_seconds` and `sickness_death_seconds` by the waking
      fraction; do **not** scale the rate `neglect_seconds` climbs. The spec
      gives both reasons and the second is the load-bearing one: neglect
      *recovers* as well as accrues (`pet.cpp:610` subtracts cared-for time back
      off it), so scaling accrual alone would make the creature harsher in a
      direction nobody asked for. **One named constant** for the waking
      fraction, because the late-night deficit lands on the same day and their
      combined pressure is a thing to feel rather than derive. **Landed**:
      `kWakingFractionNumerator`/`kWakingFractionDenominator` = 15/24, read
      through a local scaled copy so `config->sickness_onset_seconds`/
      `sickness_death_seconds` themselves are never mutated. Whether the
      combined pressure feels right is still undetermined — deferred to
      Task 7/8, per the spec's own words.
- [x] Hokorimaru needs **no compensation**: the dust branch averages need levels
      over the whole child stage (`pet.cpp:328`), the needs keep decaying while
      asleep, and the stage clock keeps running, so that average never notices
      the neglect clock stopping. Verified in the spec by reading the code.
      `hokorimaru_check` must pass unchanged — if it does not, the compression
      leaked somewhere it should not have. **Confirmed**: not one line of
      `hokorimaru_check` changed, and it passes.
- [x] Offline is computed **analytically**, never stepped. A fortnight in a
      drawer is whole days plus two partials, from Task 3's function.
      `pet_offline_ageing_check` gains cases that span nights. **Landed**:
      three new cases in `run_pet_check()` — a span starting mid-night, one
      ending mid-night, and one covering three whole nights — each proving
      both the offline/direct-`kf_pet_advance()` equivalence AND, separately,
      that night hours were actually excluded (a strict-inequality check
      against a no-clock comparison pet), since equivalence alone would pass
      even if sleep did nothing at all.
- [x] **The egg has no sleeping art** (finding 1). Decide and comment: either
      eggs do not sleep, or they sleep and simply keep showing `egg_idle_*`.
      Either is fine; an uncommented silent fallback is not. **Decided: eggs
      do not sleep.** `apply_stage_segment()`'s existing EGG early return
      (already there for "no care needed as an egg") now also means
      `state->asleep` is never touched during EGG; commented at that site
      and in ADR 0048 section 9.
- [x] `kf_creature_pose_for()` can finally return `KF_CREATURE_POSE_SLEEPING`,
      and `creature.h:47-48`'s comment saying it never does becomes **false the
      moment this lands** — fix it in the same commit. **Landed**: precedence
      is dead, sick, **asleep**, held reaction, neutral — asleep sits above
      the held reaction (a sleeping creature should look asleep even if
      `last_reaction` is still coasting) and below sick (an ill creature
      stays legibly ill overnight). The stale comment was rewritten in the
      same commit, per `CLAUDE.md`'s own rule on this.

---

### Task 7: Sleep on screen

The game's half. Uses the 18 single-frame sleeping sprites that already ship.

**Requirements:**

- [ ] `creature.lua` shows the sleeping pose, the creature settled where it
      stands, and the wander stopped. A sleeping creature that keeps walking is
      the obvious bug and the parity check will not catch it.
- [ ] **The drowsy cue** is the signal that tucking in is available. It is a
      nicety, not a duty; skipping it costs nothing beyond the spec's
      next-day deficit.
- [ ] Bedding: put away by the creature itself in the morning, per the spec.
      **There is no bedding art.** Either generate it or draw it as boxes and
      say so — do not ship a tuck-in interaction that shows nothing.
- [ ] Waking it deliberately is allowed and costs happiness. Small.
- [ ] **A sleeping creature is a static frame** — the sleeping sprites are
      single-frame (finding 1). Note it for Chris as an art decision, with the
      cost: a breathing loop is 6 stages x 3 directions x N frames of
      generation. It is not this task's job to decide.
- [ ] Chris judges bedtime feel on the board before this closes.

---

### Task 8: The attention signal

**Requirements:**

- [ ] `kf_pet_wants()` in Core, as specified in the answer to question 5: a pure
      query, a small enum, a priority order, and **hysteresis** — a want that
      switches on and off across a threshold boundary is worse than no want at
      all. Bound to Lua as `pet.wants()` returning a lowercase-free string name
      (`"FOOD"`, `"PLAY"`, …) or `nil`, so the script never handles an integer
      enum.
- [ ] **No want fires while asleep.** Task 6 runs first for exactly this reason.
- [ ] The three presentation layers in `creature.lua`: pose and position, the
      1 Hz pulsing `!`, and the inverted care-guide entry naming the button.
      All three are scene setters; no new Core drawing. **Before this can
      draw `!`, add the glyph.** `hakoniwaos/src/font_data.h`'s `0x21 '!'`
      row is currently all zeroes — add `!` to `GLYPHS` in
      `tools/make_font.py` and regenerate `font_data.h`
      (`python3 tools/make_font.py > hakoniwaos/src/font_data.h`) as the
      first step of this task. Do not substitute a different existing glyph
      without checking with Chris first; the font's character set is
      otherwise unchanged.
- [ ] A check that a hungry pet reports `FOOD`, that feeding clears it, and that
      a need hovering at the threshold does **not** produce a want that changes
      on consecutive frames. The last one is the hysteresis assertion and it is
      the only one that can fail subtly.
- [ ] **Nothing here waits on audio or haptics.** `kf/hal/audio.h` does not
      exist; the buzzer is on the target spec and is not built. Record in the
      ADR that the future sound hook is the `NONE → something` transition and
      that it is one call site, then stop.

---

## What this plan deliberately does not do

- **It does not delete LVGL.** Task 2 makes it optional and reclaims the 256 KB;
  removing the dependency retires `pet_screen_check` and `lvgl_determinism_check`
  and forecloses the evaluation `CLAUDE.md` names. Chris's call, separately.
- **It does not add a Lua execution-time limit.** Risk 6, Task 9 of the Lua
  plan. It gets worse with every screen Lua owns and this plan hands it two
  more, so it should not slip much further.
- **It does not move the wander into Lua.** Still open from ADR 0043, still
  needs a bit-exact `kf/rng.h` reimplementation to keep
  `run_lua_vs_cpp_screen_check()` meaningful, and mixing it into a plan that is
  already changing screens, the clock and sleep would make any divergence
  unattributable.
- **It does not learn the time automatically.** Chris wants WiFi sync
  eventually — *"Eventually I do want to hook it to the internet so it can tell
  what local time is. Feature for later though."* Task 4 has the owner set the
  clock by hand on the Settings screen, and the RTC then holds local time
  directly. There is no offset anywhere; see "Timezone: settled by Chris".
- **It does not implement sleeping by ambient darkness.** The spec explicitly
  defers it: *"Clock first."* The sensor is on the target spec, needs a
  simulator fallback, and is a larger build than the clock-driven version.
- **It does not answer the `1:FEED` question.** The care guide still names
  keyboard keys the device does not have.
  `2026-08-11-hardware-bringup.md:225-252` lays out four options and says it is
  Chris's call. Task 8's inverted-guide-entry work makes the labels *more*
  visible, which may be the nudge that finally forces the decision — but this
  plan ships whatever is in the tree.
- **It does not generate any art.** Sleeping poses are single-frame, there is no
  bedding art, and there is no `adult_*` family in the pack at all. Named so
  none of the three is mistaken for something this plan broke.

---

## Timezone: settled by Chris, 2026-08-13

**The user sets the clock by hand. The RTC holds LOCAL time directly.**

No timezone database, no stored UTC offset, no conversion. Whatever the owner
dials into the Settings screen is what the device believes the wall clock says,
and sleep's 22:00-07:00 window compares against it directly. On a device with
no network and no location, "local" can only mean "what the person holding it
told us", and pretending otherwise adds a conversion layer with nothing behind
it.

**Internet time sync is a wanted feature, later** — Chris's words: *"Eventually
I do want to hook it to the internet so it can tell what local time is. Feature
for later though."* The board has WiFi, so this is real future work, not a
hypothetical.

**What that means for anyone building on `kf/clock.h` now:**

- Do not add an offset field "for later". A field nothing sets is a field that
  is wrong the first time something reads it, and the save format has to carry
  it forever once it ships.
- Do keep the epoch-to-civil conversion honest and total, so that when a sync
  arrives it only has to set the clock — not reinterpret what every stored
  timestamp meant.
- The one thing worth designing against now: a sync will eventually move the
  clock *while the pet is alive*. Anything that assumes time only moves forward
  at one second per second, or that the wall clock never jumps, will break then.
  That is not a reason to build for it today; it is a reason not to write a
  comment claiming it cannot happen.
