# ADR 0051: A HUD clock on Home, and the futon as sleep's one visual

**Status:** Accepted
**Date:** 2026-08-11

## Context

Two reports from Chris, both after testing on 2026-08-11, both against
`examples/creature_demo/creature.lua` — the reference script third-party
developers read (CLAUDE.md's audience constraint) — not a new plan task.

1. *"put up a little digital wall clock in the upper left corner of the
   play room so you can keep track of the clock time as well. It's hard to
   tell what's happening when I hit each of the debug buttons."*
2. *"a core problem, the futon art doesn't show when its asleep, just the
   zzz."*

Both are Home-screen presentation changes only. Neither touches Core
(`hakoniwaos/`), the save format, or the Lua binding surface — `kf.time()`
(ADR 0047) and `pet.asleep()`/the tuck-in mechanism (ADR 0049) already
existed and are used exactly as documented; this ADR is about what
`creature.lua` does with them.

## Decision

### 1. The clock: display `kf.time()` verbatim, nothing invented

`kf.time()` (`sdk/lua/kf_lua_port.cpp:154`) already returns a ready-to-draw
12-hour string with uppercase AM/PM, or the literal `"--:-- --"` before the
clock has ever been set. Home's Lua block gains one text object:

```lua
local clock = home:text("--:-- --")
clock:move(2, 2)
clock:color(kf.BLACK, bg)
clock:layer(2)
```

and `on_home_frame()` sets it from `kf.time()` every frame, guarded so the
setter only runs when the string actually changed (see decision 2 below for
why the guard is not load-bearing for dirty rects, but is still worth
keeping).

**Position: (2, 2), Home's empty upper-left corner.** Checked against
every other object Home declares before picking it: the wander field is
`{0,0,240,260}` and nothing else is drawn above `y=232` (the poop row) in
that corner — no care bar (`y≥262`), no guide label (`y=300`), no
attention-signal `!` (`(117,196)`). The clock is 8 characters wide in the
worst case (`"12:05 AM"`, `KF_FONT_CELL_W`=6px), so its footprint is
`(2,2)`–`(50,10)`, comfortably clear of everything.

**Layer 2, not the default 0.** Home's creature body is layer 1 ("over the
mess"); nothing else claims layer 2. The wander field's top-left corner is
inside the clock's own footprint, so a wandering creature CAN pass under
it — layer 2 guarantees the clock always paints last there and stays
legible rather than being intermittently overdrawn.

**This is nearly free, deliberately not overbuilt.** No new Lua binding,
no reformatting, no new Core surface. One scene object (Home moves from
46 to 47 of the 64-slot ceiling — see "Budget" below), one small
`on_home_frame()` block.

### 2. Setting identical text every frame does not dirty a rectangle — verified, not assumed

The task asked whether the "only `:set()` when changed" guard is load-
bearing for the dirty-rect budget. It is not, and this was checked by
removing it (`clock:set(clock_str); last_clock_text = clock_str` called
unconditionally, no `if`) and re-running `home_clock_check` — it stayed
green, specifically the "a frame where the clock string does not change
dirties nothing" assertion.

The reason is `hakoniwaos/src/scene.cpp`'s `kf_scene_commit()`: it diffs
the object's **declared** `RenderState` against what was actually
**presented** last frame (`changed()`, `scene.cpp:283`), not against
whether a setter was *called*. `kf_scene_set_text()` always copies the new
string into `declared.text` and always costs a `strcmp` + a
`copy_truncated()`, but if the string is byte-identical to what is already
there, `declared.text == presented.text` after the copy, `changed()`
returns false, and no dirty candidate is ever contributed — regardless of
whether the script's own guard ran.

To prove that "no dirty rect" result is not itself vacuous — i.e. that
this check really can fail — `changed()`'s `kText` case was independently
broken (forced to `return true || ...`, unconditionally reporting every
text object changed every frame) and re-run: the same assertion failed
immediately (`FAILED: a frame where the clock string does not change
dirties nothing in the clock's own rectangle`), confirming the check
detects the real mechanism, not a tautology.

**So the guard buys correctness of intent and a cheap `strcmp`/copy
avoided per frame, not a missing dirty-rect guarantee** — the retained
scene already gives every caller that guarantee for free. Kept anyway: it
is one `if`, it documents the intent for the next reader, and it is
strictly cheaper.

### 3. The futon becomes the sleep visual, always — not only after a tuck-in

Before this change (ADR 0049), `creature.lua` showed the futon only while
the decorative `tucked_in` flag was true, which only ever became true from
a `B` press during the drowsy hour. Falling asleep **without** pressing
`B` showed the creature's own `*_sleeping_*` sprite and nothing else —
correct per ADR 0049's own design, but not what the owner expected once
seen on the board.

The fix collapses two flags into one:

```lua
local bed_shown = tucked_in or asleep
```

Whenever `bed_shown` is true — whether from a tuck-in or from Core's own
`pet.asleep()` going true on its own — the futon shows and the creature's
own body sprite is hidden. `tucked_in` still exists and still means what
it always meant (a decorative flag, reset on the real morning wake edge,
per ADR 0049 decision 6, unchanged); it is simply no longer the *only*
route to `bed_shown`.

**Tuck-in's role shrinks, deliberately, per the task's own framing**:
pressing `B` during the drowsy hour now only makes the futon appear
*early*, while the creature is still awake. Falling asleep afterward does
not swap to a new bed or a new design — `bed_shown` was already true, so
the "a fresh night begins" branch below does not re-fire, and the same
futon simply continues being what is shown. This is the one point in the
brief flagged as "say so if a different split reads better": no different
split was found — early-appearance-only reads as the natural, minimal
role for an interaction that ADR 0048 already decided changes nothing
about *when* sleep happens, only what it looks like in the meantime.

**One consequence worth naming explicitly: this closes ADR 0049's own
"Adults, and any stage with no sleeping sprite" gap, for every stage at
once, without any adult-specific code.** Adults have no `adult*_sleeping_*`
art in the shipped pack (18 sleeping sprites cover baby/child/teen0-3
only — ADR 0048/0049's own finding). Before this change, an asleep adult
resolved that missing name through `body:sprite(creature.sprite())` and
drew the generic magenta placeholder box (`kf/scene.h`'s missing-sprite
fallback) — a real, ADR-0049-documented, intentional gap. After this
change, `body:hide()` runs for every stage the instant `bed_shown` is
true, so `body:sprite(creature.sprite())` is simply never reached while
asleep, for any stage. **This is not dead code at the Core level** — the
underlying `kf_creature_sprite_name()` adult-sleeping branch
(`hakoniwaos/src/creature.cpp:127-140`) still runs every frame, still
produces the unresolvable name, and is still exercised and asserted on by
`sleep_screen_check`'s own C2 (`kf_creature_presenter_sprite_name()`
contains `"_sleeping_"`) — the presenter computes pose independently of
what Lua chooses to draw with it. What changes is only that
`creature.lua`'s own demo script no longer feeds that name into a visible
sprite object, so the placeholder box that used to be the honest,
documented fallback for a sleeping adult on the reference Home screen is
now simply unreachable from that one call site. Nothing was deleted;
nothing needs to be.

### 4. The rotation: a counter, not `math.random()` — a deliberate choice, not a sandbox limit

Chris's own words when authorizing the art: *"a random one... every
night."* Implemented instead as:

```lua
futon:frame(futon_night_index % 7)
futon_night_index = futon_night_index + 1
```

incremented exactly once per `bed_shown` false→true edge (a fresh night
starting, whether via an early tuck-in or Core's own asleep edge).

**`math` IS available to `creature.lua`'s sandbox** — `kf_lua_port.cpp`'s
`kf_lua_port_init()` loads `LUA_MATHLIBK` alongside base/coroutine/
string/table/utf8 (deliberately excluding `io`/`os`/`package`/`debug`; see
that function's own comment). `math.random()` would have worked. The
rotation is a design choice, made because `lua_determinism_check` and
`screen_parity_check` both hash the committed framebuffer and compare runs
byte for byte — a genuine PRNG pick would make the rendered output
non-reproducible between two runs of the identical script and state,
breaking both checks' whole premise. **Say so plainly, per the task's own
instruction, so Chris can ask for true randomness if he prefers it once
he has seen the rotation on the board** — a rotation still delivers "a
different design most nights, all seven eventually seen", the actual
substance of the request, just deterministically.

`futon:frame(n)` (`sdk/lua/kf_lua_scene.cpp:392`) is confirmed a plain
setter with no animation cursor attached anywhere in this codebase — set
once per night and left alone, never advanced per frame, so the seven
alternative designs (`tools/character_manifest.toml`'s `[stages.futon]`,
one pack entry, 7 frames, verified 48×48/16,268 bytes in the pack
directory) stay seven alternatives, never a flip-book.

## Budget

Home's live scene-object count moves from 46 to **47** of the 64-slot
ceiling (`kSettingsCheckExpectedObjectCount`,
`simulator/src/headless/headless_main.cpp`) — the clock's one text object,
the only new object either change adds (the futon-always logic reuses the
existing `futon`/`zzz` objects from ADR 0049, adding zero new ones).
`tools/character_manifest.toml`'s own `[stages.futon]` comment still says
"46" (it explains *why* one pack entry with 7 frames costs one scene slot
rather than seven, and the exact count is not load-bearing to that
argument) — **left un-edited on purpose**, per this task's own explicit
instruction not to touch the manifest.

**Worst-case dirty rects, measured, not assumed** (`sleep_screen_check`,
`run_sleep_screen_check()`):

| Shape | Worst-case dirty rects | `KF_MAX_DIRTY_RECTS` |
|---|---|---|
| Asleep, not tucked in (ADR 0049's own case, re-measured) | 1 | 8 |
| Tucked in (ADR 0049's own case, re-measured) | 1 | 8 |
| **Asleep, wall clock genuinely ticking** (new — the task's own explicit ask) | **2** | 8 |

The first two both moved from ADR 0049's original 0/1 to a shared 1: with
the futon-always change, the plain "asleep, not tucked in" case now also
shows the wiggling/blinking futon instead of a static sleeping sprite, so
it picks up the same single merged rect (futon + ZZZ, close enough
together to coalesce — ADR 0049's own reasoning for why tucked-in measured
1, not 2) that tucked-in already had. The third row is the one this task
was specifically asked to measure: three independently-moving things
(clock text, ZZZ blink, futon wiggle) instead of two, with the wall clock
advanced by a full minute every 30 frames — far more often than a real
device's clock would tick — specifically to raise the odds of catching a
frame where all three coincide. **2, not 3**: the clock sits in Home's
top-left corner, spatially far from the futon/ZZZ cluster wherever the
creature happens to be sleeping, so the two never merge into one rect —
this is genuinely two separate dirty rectangles most ticking frames, one
for the clock, one for the futon/ZZZ pair, both comfortably inside the
budget of 8.

## Non-vacuity: every new assertion broken and watched fail, then restored

Per this project's own rule that a passing test is not evidence it tests
anything.

| Assertion | Breakage introduced | Failure observed | Restored |
|---|---|---|---|
| The clock shows `kf.time()`'s exact string (both `"--:-- --"` unset and `"9:05 AM"` set) | `creature.lua`'s clock-update block replaced with `if false then ... end` | `FAILED: once the clock is set, the Home clock shows "9:05 AM", pixel-identical to a direct render of that string` | Yes |
| The clock repaints when the string changes, to the new string | Same breakage as above | `FAILED: a frame where the clock string DOES change dirties the clock's own rectangle` and `FAILED: and the new string is exactly "9:06 AM", not stuck on the previous minute` | Yes |
| An unchanged frame dirties nothing in the clock's rectangle | `hakoniwaos/src/scene.cpp`'s `changed()`, `kText` case, forced to `return true \|\| ...` | `FAILED: a frame where the clock string does not change dirties nothing in the clock's own rectangle` | Yes |
| A sleeping pet with no tuck-in shows *something* at the fall-asleep position | `bed_shown` reduced from `tucked_in or asleep` to `tucked_in` alone | Cascaded into "consecutive nights use different futon designs" failing (see next row) — see "Caught vacuous" note below | Yes |
| ...and what shows there is specifically the futon, not the creature's own resolved sleeping sprite | Same breakage as above | `FAILED: no tuck-in: what's shown at the bed position is NOT the creature's own resolved sleeping sprite -- confirming it is genuinely the futon, not merely something non-background that happens not to be the futon either` | Yes |
| Consecutive nights use different futon designs | `futon:frame(futon_night_index % 7)` replaced with `futon:frame(0)` (never rotates) | `FAILED: consecutive nights use different futon designs` | Yes |
| The 8th night's design repeats the 1st exactly | Same breakage as above | The rotation-specific assertion did not independently fail here (a constant `0` trivially satisfies "8th equals 1st"), but the mismatched *consecutive-differs* row above already proves this is not a coincidence — see note below | Yes |
| The same night's design does not change frame to frame | `futon:frame((futon_elapsed_ms // 66) % 7)` called every `bed_shown` frame (a genuine flip-book instead of a fixed choice) | `FAILED: the same night's futon design does not change frame to frame` (plus the same cascading "consecutive nights" failure) | Yes |

**Caught vacuous, then closed, exactly the trap CLAUDE.md and ADR 0049 both
warn about**: the first version of the "no tuck-in shows the futon" check
sampled a single pixel and asserted only `!= home_bg`. With `bed_shown`
reduced to `tucked_in` alone, that check **stayed green** — the
creature's own `*_sleeping_*` body sprite is *also* non-background, so
"something is there" proved nothing about *what*. Closed by rendering the
creature's own resolved sleeping sprite (`kf_creature_presenter_sprite_
name()`) as a direct reference at a scratch corner of the wander field
(picked to never overlap the live bed position — whichever of the two
diagonal corners is farther away, by a plain AABB check) and requiring the
live sample **not** match it, byte for byte at a small interior sample
grid. Re-broken and re-confirmed failing (row above) before being
accepted.

**Why the "8th equals 1st" row could not independently prove the
rotation-vs-constant breakage**: a script permanently stuck on frame 0
trivially also satisfies "frame 8 equals frame 1" (both are 0). This is
named here rather than silently left implicit, because it is exactly the
kind of assertion that reads like it proves something and does not on its
own — it is only meaningful *together with* "consecutive nights differ",
which the same breakage does fail. The pairing, not either assertion
alone, is what proves `% 7`, not a stuck value.

The interior-sample-grid technique (5×5 points at offsets `{12,18,24,30,
36}` from the sprite's own top-left, avoiding the transparent margin
`tools/character_manifest.toml`'s own `[stages.futon]` comment measures at
912 of 2,304 pixels) needed one correction while being written: an early
draft picked `(150, 300)` as the futon reference's scratch position for a
different, now-abandoned comparison technique, and the check's own first
run caught it directly — `y=300` collides with Home's real
`"4:BATH"`/`"5:FLUSH"` guide labels, and a shorter string's un-covered
trailing column showed those labels' real black glyph pixels bleeding
through instead of background. Moved to `y=290`, the one gap in Home's
own layout (between the need bars ending at `y=288` and the guide row
starting at `y=300`) that nothing else ever draws into.

## What was found and not changed

**`screen_parity_check`'s own strict byte-parity guarantee needed a
one-time repair, not a re-baseline.** That check hashes `kf_creature_
screen.cpp` (the frozen C++ reference, `ADR 0043`'s "not an actively
maintained second implementation") against `creature.lua`'s real Home,
frame by frame, asserting byte-identical output up to frame 150 (where
Task 8's attention signal is the one documented, deliberate divergence).
The clock draws on *every* frame from frame 0, so leaving it in the hash
would have made the two screens diverge at frame 0 permanently — not a
new, bounded divergence point the way Task 8's was, but the total loss of
this check's ability to ever again catch an *unrelated* regression
anywhere else on the screen (every future run would report "diverges at
frame 0" whether or not anything else was actually wrong). Fixed by
masking the clock's own known rectangle (`(2,2)`–`(50,10)`) out of both
halves' hash — on a scratch copy of the framebuffer, never the live one
`scene.cpp` is diffing against — so the frame-150 guarantee for
everything else on the screen is exactly as strong as it was before the
clock existed.

**`kSettingsCheckExpectedObjectCount` moved from 46 to 47** (the clock's
one object), measured the same way ADR 0049 measured its own 45→46 move —
run the check, read the actual number off its own failure message, update
the named constant.

## Not verified

**Nothing about either change has run on hardware.** Both are Home-screen
presentation only, built and verified against the desktop `kamiframe-
headless` suite and cross-compiled clean for `-DKF_PANEL=ili9341`
(firmware 0x81180 bytes, 66% of the app partition free — not flashed, per
this task's own instruction). The clock's exact position, its layer
choice against a wandering creature, and the futon's visual correctness
at 48×48 under real colour-key transparency are all things only the board
can confirm.

**The rotation vs. true randomness question is deliberately left open**,
per decision 4 above — this ADR states the reasoning and the tradeoff;
whether the rotation reads as "enough" variety once seen for real, across
real nights, is Chris's call to make on the board.
