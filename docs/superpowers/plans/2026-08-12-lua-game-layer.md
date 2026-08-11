# The Lua Game Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The demo pet stops being a C++ program that a Lua script comments on,
and becomes a Lua program. Every pixel it puts on screen is declared by a script
a WordPress or jQuery developer could read, edit, and copy. HakoniwaOS keeps the
blitter, the dirty rectangles, the frame budget and the pet simulation; the game
keeps everything a game is: which sprite, where, which pose, which screen, what
a button does.

**Architecture:** **Retained mode.** Lua declares what exists; the engine diffs
this frame's declaration against last frame's, computes the dirty rectangles,
coalesces them under `KF_MAX_DIRTY_RECTS`, and repaints only what changed. The
scene and the differ live in `hakoniwaos/` as plain C, heap-free and float-free,
so they run identically on the ESP32 and can be tested — and can render the
existing screen — with no Lua involved at all. The Lua binding is a thin
wrapper over that Core API. This split is what lets the C++ screen and the Lua
screen be diffed against each other pixel for pixel rather than asserted about.

**Tech Stack:** C++17, C for the Core scene module, Lua 5.5.0 (fetched, not
vendored — `cmake/fetch_lua.cmake`), CMake + ESP-IDF, CTest via
`kamiframe-headless --verify-*` check modes, stdlib Python for the embed tool.

## Status: Tasks 1-6 COMPLETE (wander migration deferred) — see the status
table near the end of this document, added 2026-08-12

Written 2026-08-12 as NOT STARTED; that has since changed and is corrected
here rather than left standing. Every figure in "What is true today" below
was measured in this worktree on this machine on 2026-08-12, not copied from
an ADR, and reflects the plan's starting point, not the current tree.

---

## READ THIS BEFORE DISPATCHING ANY TASK FROM THIS PLAN

**This project's plan documents have manufactured five defects by being copied
verbatim.** Three were comments contradicting their own code; one was a real
`ValueError` in a listing; one was a function called four times against an
assert that fires on the second call, which cost two implementers time. See
`2026-08-10-animated-indexed-sprites.md`'s identical banner.

This plan therefore keeps code listings **minimal and load-bearing only**. Where
an exact snippet is not required for correctness, the step states the
requirement and names the file, and the implementer writes it. The two listings
that do appear — the minimal-pet Lua script and the `kf/scene.h` function list —
have been reasoned through line by line against names verified in the tree.

Two rules follow, and the second matters more:

1. When a review finds a bad comment or pattern, grep **this file** as well as
   the source tree. A defect here costs one defect *per remaining task*.
2. **Update this plan when a decision is made, before dispatching the task it
   affects.** Briefs are generated *from* this file, so a stale line here is
   re-served to every implementer that follows.

---

## What is true today, measured in this worktree on 2026-08-12

| Claim | Verified how | Result |
|---|---|---|
| Desktop suite green | `ctest --test-dir build -N` | **40 tests**, all defined in `simulator/CMakeLists.txt` lines 263–777 |
| Lua's whole API | `simulator/src/lua/kf_lua_port.cpp:62`, `:300` | **24 functions: 2 in `kf`** (`log`, `report`) **and 22 in `pet`** — not 24 in `kf` |
| Lua cannot draw | grep for a drawing binding | **Nothing** exposes `kf_fill_rect`, `kf_blit_frame`, `kf_text_draw`, the framebuffer or `kf_creature_*` to Lua |
| The demo script | `simulator/src/lua/kf_lua_demo_creature_script.h` | 147-line header; **~91 lines of Lua** inside an `R"lua(...)lua"` literal, lines 47–139 |
| Script entry points | `kf_lua_port.cpp:393`, `:441` | The top-level chunk, run once; then `on_frame(dt_ms)`, fetched by `lua_getglobal`. **That is all.** No `init`, no `on_draw` |
| C++ game logic | `wc -l` | `kf_creature_screen.cpp` **1,364**, `hakoniwaos/src/creature.cpp` **286**, `kf_screen_nav.cpp` **152**, `kf_pet_info_screen.cpp` **183** — **1,985 lines** |
| `/sdk` | `ls sdk` | **Does not exist.** `CLAUDE.md`'s repo layout lists it |
| Sprite names in the shipped pack | parsed `examples/creature_demo/assets.kfpack` | 94 entries, 556,488 bytes: `egg_idle_{s,e,n}`, `{baby,child,teen0..3}_{happy,neutral,objecting,sick,sleeping}_{s,e,n}`, `shrine_idle_s` |
| Text in Core | `hakoniwaos/include/kf/font.h` | `kf_text_draw()` / `kf_text_width()`. 5x7 in a 6x8 cell. **Uppercase only**, plus `0-9` and `. , : - / % + ( )`. No transparent mode |
| Dirty-rect merging | `hakoniwaos/src/framebuffer.cpp:43` | `touches_or_overlaps()` expands by 1px, so the creature's erase and redraw already merge into one rect |

### Six things the premise of this work got wrong or did not know

Two of these would have sent a task to the wrong file. One would have made a
correct implementation fail a test for a reason nobody would have expected.

**1. `kf_screen_nav.cpp` and `kf_pet_info_screen.cpp` are not in
`simulator/src/pet/`.** They are in **`simulator/src/lvgl/`**, and that is not a
filing accident — the Info screen is a pure **LVGL widget tree** with no
framebuffer drawing in it at all (`kf_pet_info_screen.cpp:22-30`, a struct of
`lv_obj_t *` labels). Moving Info to Lua is not a port, it is a rewrite against
a different rendering model. That is why it is late in this plan and not early.

**2. The two golden rendering checksums are not at risk, at all.**
`headless_determinism` and `headless_fullscreen` run the **default path of
`main()`** (`simulator/src/headless/headless_main.cpp:6078-6144`) — 300 frames
of `kf_app_frame()` with Core's own demo (`KF_DEMO_SPRITE` and
`KF_DEMO_FULLSCREEN`, `hakoniwaos/include/kf/demo.h`), hashed by
`kf_display_present()` in `simulator/src/headless/headless_display.cpp:54-78`.
Neither touches `simulator/src/pet/*`. **Nothing in this plan can move them**,
and if one moves, something is wrong that has nothing to do with the intended
change. The checks this plan genuinely puts under load are the ten
creature-screen checks listed further down.

**3. `kf_lua_port.h:9-12` says Lua is "simulator-only, on purpose ... not wired
into hakoniwaos/ (core) or ports/esp32/".** It is wired into ports/esp32:
`ports/esp32/main/CMakeLists.txt:96-97` compiles `kf_lua_alloc.cpp` and
`kf_lua_port.cpp` into `main`, and `app_main.cpp:216` calls
`kf_lua_port_init()`. ADR 0028 is the decision that made it so. A comment
contradicting its code is a defect by this project's own rule. **Task 1 fixes
it**, because Task 1 is the task that touches that file.

**4. There is no `.lua` file anywhere in this repository.** Every script is a
C++ raw string literal inside a header (`kKfLuaDemoCreatureScriptSource` and
eight proof-script siblings). To change one line of the demo creature's
behaviour today you edit C++ and recompile — on the device, you reflash 660 KB
of firmware. `docs/sdk-style-guide.md:66-77` already names this: *"today there
is exactly one hardcoded demo script the simulator loads directly, no cartridge
format, no loading mechanism."* No amount of API design fixes it. **This is the
single largest gap between the stated promise and the audience constraint, and
it is Task 1** — cheapest, highest-information, and it unblocks reading and
reviewing everything that follows.

**5. `run_frame_counters_check()` will fail against a correct retained-mode
screen, and it will look like a regression.** It asserts
`keyed_pixels >= 2000 && keyed_pixels <= 8000` and `dirty_rect_count >= 1`
(`headless_main.cpp:4415` region). Today those hold because
`kf_creature_screen_frame()` erases and redraws the creature **unconditionally
every frame** (`kf_creature_screen.cpp:1128`, `:1240-1242`). A retained scene
draws nothing on a frame where nothing changed, so both assertions go to zero.
The check is right to exist and its window is right; what it needs is a frame on
which something *did* move. **Task 4 owns this**, and must fix it by making the
check drive real movement, not by widening the window to include zero — a
window that accepts zero is the vacuity trap this codebase has already been
bitten by twice.

**6. There is no execution-time guard on Lua.** No `lua_sethook`, no
`LUA_MASKCOUNT`, no deadline. `while true do end` in a script hard-hangs the
frame loop on desktop and on device. ADR 0014 (line 195) and ADR 0028 (line 190)
both name this as a known, deliberate gap. Today it costs a hung log; once Lua
owns the screen it costs a frozen device. **Named as a risk below; the fix is
Task 9 and deliberately not tonight.**

### Stale comments found while reading — defects by this codebase's own rule

Each task that touches the file fixes its own. The rest are listed so nobody
rediscovers them.

- `simulator/src/lua/kf_lua_port.h:9-12` — "simulator-only ... not wired into
  ... ports/esp32/". Contradicted by `ports/esp32/main/CMakeLists.txt:96-97`.
  **Task 1 fixes.**
- `docs/superpowers/plans/2026-08-11-hardware-bringup.md:24` — "Status: NOT
  STARTED". Its Tasks 1, 4 and 9 have landed (commits `73e76fd`, `7e2d466`,
  `b730559`, `61fae4b`, `fe06c8b`, `4fbc9fa`; ADRs 0036 and 0039 exist). Tasks
  2, 3, 5–8 have not, and ADRs 0037/0038 are absent. **Not this plan's file to
  fix, but do not read that Status line and conclude the instruments are
  missing — `KFDBG STATE`'s budget fields exist and this plan depends on them.**
- `tools/kf_panel.py:630-633` — `STATE_FIELD_ORDER` names `hunger`,
  `happiness`, `energy`, `time_in_stage_s`, `frame_time_ms`,
  `free_heap_bytes`. The firmware emits `hunger_mp`, `happiness_mp`,
  `energy_mp`, `stage_elapsed_s`, `frame_us`, `heap_free_internal`. Harmless —
  unrecognised keys are appended rather than dropped (its own comment at
  `:624-629`) — but the ordering it intends has never actually applied on real
  hardware. **Out of scope here; noted so it is not mistaken for damage this
  plan did.**

---

## Decisions already taken — record, do not re-litigate

**Retained mode, not immediate mode** (Chris, 2026-08-12). If a script calls
`draw_sprite(x, y)` every frame, Core cannot know what changed. Either
everything is marked dirty — 240x320x2 = 153,600 bytes, ~31 ms of SPI transfer
against a 33.3 ms budget, which destroys the frame rate — or the script author
manages dirty rectangles by hand. The second is unacceptable for the target
audience. So the game says *what exists*; the engine works out *what changed*.

**The scene and the differ live in `hakoniwaos/`, not in the Lua binding.**
Three reasons, and the third is the one that matters: it runs on the device
identically by construction; it is testable with no Lua in the picture; and it
lets the **existing C++ creature screen be rebuilt on top of it first**, so the
differ is proved against ten already-passing tests before a single pixel is
declared from Lua.

**The pet simulation does not move.** `CLAUDE.md` puts the pet framework in
`/hakoniwaos` deliberately: HakoniwaOS *provides* a pet, games build on it.
Whether a third-party creature should define its own needs, decay curves and
evolution rules is a real question — and a separate one. Opening the rendering
boundary and the simulation boundary in the same plan would mean that when
something behaves oddly, there are two candidate causes and no way to tell them
apart. **Out of scope. Named in "What this plan deliberately does not do".**

**The C++ screen stays as a fallback while the Lua one is proved, and the two
are diffed against each other.** Not asserted about — diffed. A headless check
runs both for N frames against the same seeded pet and compares the framebuffer
with the same FNV-1a hash the golden checksums already use. That is a far
stronger correctness argument than any hand-written assertion, and it is
available cheaply precisely because the scene lives in Core and both languages
can drive it.

**The jQuery accessor convention: no argument reads, arguments write.**
`body:x()` returns the x; `body:x(96)` sets it. One rule, applied to every
property of every object, and it is the exact idiom this audience already has in
their fingers (`$el.text()` / `$el.text("hi")`). It is why the API needs no
`get_`/`set_` prefixes and no property-bag table.

**Scripts become real `.lua` files, checked in, embedded by a generator.**
Generating the header at build time would need the generator wired into two
build systems including ESP-IDF's two-pass configure. Checking the generated
header in and adding a test that regenerates and diffs gives the same guarantee
at a fraction of the risk, and is the pattern `tools/make_font.py` already sets.

---

## The audience constraint is a hard requirement

*"A WordPress or jQuery developer should not have too much trouble getting a pet
running."* Here is how each decision serves that, stated so it can be checked
rather than claimed:

| Decision | What it buys the audience |
|---|---|
| Retained mode | They never see a dirty rectangle. They set a position; the engine works out the consequences. |
| Scene lives in Core, fixed-size static array | No `malloc`, no `free`, no ownership. Objects live until `:remove()`. |
| No-arg reads, args write | One rule for every property. Nothing to memorise per function. |
| Sprites addressed by **name**, never by index or pointer | No frame indices, no palette handles, no byte order, no colour keys. `kf.sprite("baby_neutral_s")`. |
| A missing sprite draws a magenta box **and** logs the name | The failure is visible on the screen *and* named in the console. It never silently draws nothing. |
| `kf.sprites()` lists every name in the mounted pack | A typo is self-serviceable: print the list, find the real name. |
| `kf.color(r, g, b)` plus named constants | No RGB565 bit-packing. Ever. |
| `on_frame(dt_ms)` is the only lifecycle callback, and it already exists | Zero new concepts in the part they hit first. |
| A script error leaves the **last good frame** on screen | A mistake costs a frozen pet and a log line, not a black screen or a crash. |
| The whole minimal pet is 14 lines | Stated as a requirement below, with the listing. If it grows, the API is wrong. |

---

## Global Constraints

Every task's requirements implicitly include this section.

- **`hakoniwaos/` stays heap-free.** `python3 tools/check_no_heap.py .` runs in
  `bash dev.sh test` and fails the build. The scene is a file-static
  fixed-capacity array in `hakoniwaos/src/scene.cpp`. Lua's own 1 MB heap comes
  from `kf_arena_alloc(KF_ARENA_LUA, KF_ARENA_LUA_BYTES, ...)`, called exactly
  once ever (`kf_lua_alloc.cpp:217`); Core allocates nothing.
- **`hakoniwaos/` stays free of floating point.** No `float`, no `double`.
  Lua is built with `LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` and, on the ESP32,
  `LUA_32BITS` — so **every numeric binding takes `luaL_checkinteger`, never
  `luaL_checknumber`**, and the C side sees only integers.
- **240x320 RGB565, no alpha anywhere.** Transparency is colour-key only,
  magenta `KF_RGB(255,0,255)`. Sprites are 48x48.
- **Maximum 8 dirty rectangles per frame** (`KF_MAX_DIRTY_RECTS`,
  `hakoniwaos/include/kf/framebuffer.h:44`). Past 8 the framebuffer collapses to
  one screen-sized box and re-transfers ~31 ms against a 33.3 ms budget. The
  current worst case is 5 (`run_creature_screen_budget_combination_check()`,
  `headless_main.cpp:4170`). **Staying under 8 is the engine's problem and must
  never become the script's** — see "Coalescing" below for the mechanism.
- **The desktop build is the real firmware against a desktop HAL backend.**
  Everything in `hakoniwaos/` runs identically on the ESP32, and
  `simulator/src/pet/kf_creature_screen.cpp` is compiled into the device build
  too (`ports/esp32/main/CMakeLists.txt`). Nothing here is desktop-only unless
  it says so.
- **Do NOT run `cmake -B build`.** It is already configured; reconfiguring costs
  ~2 minutes. Build with `cmake --build build -j8`, test with
  `ctest --test-dir build`. **Desktop baseline was 40/40 when this plan was
  written; run `ctest --test-dir build -N` to get today's number before
  starting** (44/44 with the default `KF_ENABLE_LVGL=OFF` as of 2026-08-11,
  46/46 with it ON — both will have moved further since). Whatever the
  count is at dispatch, it must stay that count plus whatever a task adds.
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

  `ili9341` is the panel currently connected. Sourcing `export.sh` also puts
  pyserial 3.5 on the path, which system python3 lacks.
- **The two golden rendering checksums must not move**, and — see finding 2
  above — nothing in this plan should be able to move them. If one does, stop
  and find out why; do not re-baseline.
- **`kf/budget.h`'s banner forbids moving a budget number to make a test pass.**
  It does not forbid replacing an assumption with a measurement.

---

## The retained scene, concretely

### What an object is

A scene object is one of three kinds — **sprite**, **text**, **box** — plus a
scene-wide **background** (a colour or a full-screen sprite). Every object
carries: position, visibility, layer, and its own kind-specific payload
(sprite name + frame + mirror; string + fg/bg colour; width/height + colour).

The scene holds them in a file-static array in `hakoniwaos/src/scene.cpp`, of
capacity `KF_SCENE_MAX_OBJECTS`. Start at **64** — the whole current home screen
is under ten objects (creature, up to four poops, three stat bars, three stat
labels, five care-guide labels), so 64 leaves an order of magnitude of headroom
against 64 * ~40 bytes ≈ 2.5 KB of static storage. Creating the 65th raises a
Lua error naming the limit; it does not return `nil` for the script to
dereference into a confusing "attempt to index a nil value".

### How the diff works

`kf_scene_commit()` runs once per frame, after the game has finished declaring.
For each object it compares the **declared** state against the **presented**
state (what was last actually painted) and, if any field differs, contributes
the object's old rect and its new rect as dirty candidates. It then:

1. Coalesces the candidate set down to at most `KF_MAX_DIRTY_RECTS` (below).
2. Marks each final rect with `kf_fb_mark_dirty()`.
3. For each final rect, repaints the background then every visible object whose
   bounds intersect that rect, in layer order (ties by creation order).
4. Copies declared state to presented state.

**A frame in which nothing was changed produces zero dirty rectangles and draws
nothing.** That is the headline correctness property and the headline
performance win, and it is what the check in Task 2 asserts first.

### Coalescing — the mechanism that keeps the budget invisible

`kf_fb_mark_dirty()` already merges rects that touch or overlap, and past 8 it
collapses *everything* into one screen-sized box (`framebuffer.cpp`, and the
header says so at `:64-68`). Collapsing is never wrong, only ever expensive —
and expensive here means ~31 ms of a 33.3 ms frame.

The scene knows all its candidate rects **before** marking any, which the old
call-as-you-draw path never did. So it can do better: while the candidate count
exceeds `KF_MAX_DIRTY_RECTS`, repeatedly merge the pair whose union adds the
least area, then mark the survivors. Worst case that is two rects merged into a
slightly larger one; the framebuffer's own fallback is the whole panel. The
candidate working set is a second file-static array — cap it at
`KF_SCENE_MAX_DIRTY_CANDIDATES = 32` and merge eagerly past that, so nothing is
ever on the stack and nothing is ever unbounded.

**Known cost, stated rather than discovered:** step 3 repaints a whole object
even when only part of it falls inside the rect, because `kf_blit_frame()` clips
to the screen and not to an arbitrary rectangle. The overdrawn pixels are inside
the union of an already-marked rect and an intersecting object's bounds, so
correctness holds and the rect count does not grow — the extra marks union into
the rect they touch. The cost is pixels, not rectangles. A `kf_clip_push()` /
`kf_clip_pop()` pair in Core is the clean fix if measurement ever asks for it;
it is not built here because it would change every existing blit call site to
buy something nothing is currently short of.

### The API, in Lua

The `kf` table gains — alongside the existing `log` and `report`:

| Call | Does |
|---|---|
| `kf.sprite(name)` | Creates a sprite object showing pack entry `name`. |
| `kf.text(str)` | Creates a text object. |
| `kf.box(w, h, color)` | Creates a filled rectangle. |
| `kf.background(color_or_name)` | Sets the scene's base layer. |
| `kf.color(r, g, b)` | Packs 0–255 components into the engine's colour value. |
| `kf.WHITE`, `kf.BLACK`, `kf.RED`, `kf.GREEN`, `kf.BLUE`, `kf.YELLOW` | Named constants, so the first script needs no colour maths at all. |
| `kf.on_button(name, fn)` | `"a" "b" "menu" "up" "down" "left" "right"`. Fires on the press edge. |
| `kf.width()`, `kf.height()` | 240, 320. So nothing hardcodes the panel. |
| `kf.sprites()` | Every entry name in the mounted pack, as a list. A typo becomes self-serviceable. |

Every object supports, with the no-arg-reads / args-write rule throughout:

`:move(x, y)` · `:x([n])` · `:y([n])` · `:show()` · `:hide()` · `:visible([bool])`
· `:layer([n])` · `:remove()`

Sprites add `:sprite([name])` and `:flip([bool])`. Text adds `:set([str])` and
`:color([fg[, bg]])`. Boxes add `:size([w, h])` and `:color([c])`.

`obj:sprite(name)` and `kf.sprite(name)` share a word deliberately: the receiver
distinguishes them, exactly as jQuery distinguishes `$.ajax` from `$el.attr`.
The alternative — inventing a second verb for "change which picture" — costs a
word the audience would have to learn for no gain.

### Failure behaviour, decided up front

| Mistake | What happens |
|---|---|
| `kf.sprite("typo")` | The object is created and draws a **magenta placeholder box** — the same `kPlaceholderColor = KF_RGB(255,0,128)` the C++ screen already uses at `kf_creature_screen.cpp:88` — and `kf.log`s an error naming the requested name. Visible on the panel, named in the console, and the pet keeps running. |
| `body:move("left")` | Lua's own `luaL_checkinteger` message: *bad argument #1 to 'move' (number expected, got string)*, with the script line. Already good; do not wrap it. |
| The 65th object | `luaL_error` naming `KF_SCENE_MAX_OBJECTS`. Raised at creation, which is normally the top-level chunk, where a hard failure is the right failure. |
| Lowercase in `kf.text` | The font has no lowercase; a blank cell per letter is exactly the silent failure the audience constraint forbids. **The binding uppercases ASCII letters** and says so in its own comment. Genuinely unsupported characters log once, naming the character. |
| An error inside `on_frame` | Logged once, `on_frame` is disabled until the next `kf_lua_port_init()` (`kf_lua_port.cpp:447-455`) — **keep that policy**. Under retained mode the consequence is benign and correct: the last good frame stays on the panel. Task 5 adds a one-line error banner in the reserved band so the panel says what the console says. |
| `while true do end` | Hangs the frame loop. **Not fixed in this plan.** See the risk table. |

---

## The minimal pet, in Lua

This is the acceptance test for the API. If it is not short and obvious, the API
is wrong and should be changed before Task 3 is dispatched.

```lua
-- A complete pet: it appears, it reacts to two buttons, it says how it feels.
kf.background(kf.color(232, 240, 216))

local body = kf.sprite("egg_idle_s")
body:move(96, 106)

local mood = kf.text("")
mood:move(4, 262)

kf.on_button("a", function() pet.feed() end)
kf.on_button("b", function() pet.play() end)

function on_frame(dt_ms)
    if pet.stage() == "egg" then
        body:sprite("egg_idle_s")
    else
        body:sprite("baby_neutral_s")
    end
    mood:set("HUNGER " .. math.floor(pet.hunger() / 1000) .. "%")
end
```

Fourteen lines of substance. Every name in it is verified: `egg_idle_s` and
`baby_neutral_s` are real entries in `examples/creature_demo/assets.kfpack`;
`pet.stage()` returns `"egg"` / `"baby"` / `"child"` / `"teen"` / `"adult"`
(`kf_lua_port.cpp:191`); `pet.hunger()` returns millipercent 0..100000
(`kf/pet.h`), so `/ 1000` is percent; `math` is one of the six enabled libraries
(`kf_lua_port.cpp:380-383`); the string contains no lowercase, so it needs no
help from the uppercasing rule. 96 and 106 centre a 48x48 sprite in the
240x260 field (`kField`, `kf_creature_screen.cpp:46`), and 262 is the first
stats row (`kStatsRowsY0`, `:661`).

Note what is **not** in it: no dirty rectangles, no `on_draw`, no clear, no
double-buffer, no frame indices, no colour key, no memory management, no `nil`
checks. That is the whole point.

---

## What moves and what stays

| Stays in Core (HakoniwaOS, C++) | Moves to Lua (the game) |
|---|---|
| Blitter, framebuffer, dirty rects, coalescing, DMA transfer | Which sprite, where, which pose |
| Asset pack loading, palettes, frame addressing | The wander, pose selection |
| The animation clock (`kf_creature_tick_anim`) | Screens, navigation, layout |
| Pet framework — needs, decay, evolution, offline ageing | What a button does |
| Input debouncing (`debounce()`, `app.cpp:83`) | Stats band and care guide composition |
| The retained scene and its differ (**new**) | The declaration fed to it |

`hakoniwaos/src/creature.cpp`'s wander (`kf_creature_update`, `choose_target`,
`kSpeedPxPerSec = 18`) is presentation, and by this table it belongs to the game.
It moves to Lua in Task 6, not before — the wander is the one piece whose
behaviour is easiest to change accidentally, and moving it while the renderer
is also new would make an A/B diff impossible to attribute.

---

## The debug surfaces: which break, per surface

The owner asked directly. Here is the answer, verified file by file rather than
reasoned about.

**`simulator/src/sdl/sdl_debug_window.cpp` — UNAFFECTED through Task 6, one
line of rework at Task 7.** It never touches the framebuffer: it owns its own
`SDL_Renderer` and draws with `SDL_RenderFillRect` / `SDL_RenderDebugText`. It
includes exactly two game headers — `kf_pet_session.h` and `kf_screen_nav.h` —
and **does not include `kf_creature_screen.h` at all**. Timeline scrubbing is
`kf_pet_session_debug_seek()`; stage jump is
`kf_pet_session_debug_jump_to_stage()`; the form and branch pickers write
file-local `Session` fields consumed by that jump; the time multiplier writes a
file-local field that `sdl_main.cpp:242` folds into `kf_pet_session_frame()`
only. Every one of those is the pet session, which this plan does not move. The
**one** coupling that does break is the "Next Screen" button and the screen
readout (`kf_screen_nav_debug_advance()` / `kf_screen_nav_debug_index()`,
`sdl_debug_window.cpp:444`, `:871`) — Task 7 must keep `kf_screen_nav_*` as the
C++-side registry of which screen is showing even once the screens themselves
are declared in Lua, and Task 7 says so as a requirement.

**`tools/kf_panel.py` — UNAFFECTED, entirely.** It speaks KFDBG over serial and
nothing else; `SocketTransport` raises on `sim:` targets because the desktop
simulator has no KFDBG server (`kf_panel.py:148`). Its screenshot path decodes
240x320 RGB565 from `KFDBG SHOT`, which is format-agnostic about who drew the
pixels. Its state readout tolerates unknown keys by design. Nothing in it knows
what a screen or a sprite is.

**KFDBG — UNAFFECTED, entirely, and it becomes the measuring instrument.** Going
through the dispatcher (`process_command_line()`,
`ports/esp32/main/kf_dbg_bridge.cpp:1279`) command by command:

| Command | Touches | Verdict |
|---|---|---|
| `PING` | build stamp, panel profile name | unaffected |
| `SHOT` | `kf_fb_pixels()` directly (`:475`) | unaffected — reads finished pixels, knows nothing about who drew them. **This is what the Lua-vs-C++ diff runs on, on hardware.** |
| `STATE` | `kf_pet_session_state()`, `kf_app_last_frame()`, `kf_app_frame_summary()`, `kf_app_post_frame_us()` | unaffected. **This is the budget instrument** — `draw_us`, `keyed_px`, `opaque_px`, `dirty_rects`, `cpu_us`, `post_us`, landed by ADR 0036 |
| `SCANLINE`, `VSYNC` | display driver | unaffected |
| `BTN`, `BTNHOLD` | `g_inject_mask`, consumed through `kf_input_poll()` → `debounce()` → `kf_app_buttons_pressed()` | unaffected **provided** `kf.on_button` reads that same debounced edge mask. That is a hard requirement on Task 3, not an assumption. |
| `ADVANCE`, `RESET`, `MULT`, `JUMP` | `kf_pet_session_debug_*` | unaffected |
| `FEED`/`PLAY`/`REST`/`BATH`/`FLUSH` | `kf_pet_session_feed()` and friends **directly**, deliberately bypassing `kf_creature_screen.cpp`'s `handle_care_buttons()` (`:1106-1158`) | unaffected — and the bypass is why. The care path never went through the screen layer. |
| `WATCH` | not a wire command; `tools/kf_debug.py:899` polling `STATE` | unaffected |

**The ten tests that genuinely break — NEEDS REWORK, at Task 4:**
`creature_screen_check`, `creature_screen_input_check`,
`creature_screen_sprite_check`, `creature_screen_egg_check`,
`creature_screen_death_check`, `creature_screen_debug_jump_check`,
`creature_screen_stats_check`, `creature_screen_budget_combination_check`,
`creature_anim_check`, `frame_counters_check`. All ten drive
`kf_creature_screen_frame()` and seven `kf_creature_screen_debug_*` hooks.
Task 4 keeps `kf_creature_screen.cpp`'s public and debug API **byte-identical**
while replacing its innards with scene declarations, so nine of the ten pass
unmodified. The tenth is `frame_counters_check` — see finding 5.

**Predicted checksum movement: none.** All the creature-screen dirty-rect
assertions are `<=` bounds (`<= 2u`, `<= 5u`), and retained mode makes those
counts fall, not rise. `pet_screen_check` (`132458f0171a2c0b`) and
`screen_nav_check` (`ac44bb9819809bea`) are LVGL screens and are untouched
until Task 8, where `screen_nav_check` moves legitimately and its constant is
re-baselined **with the reason recorded in that task's ADR**.

---

## The frame budget: what to expect, and how it gets measured

Per-call Lua→C overhead is ~1–3 µs. A retained frame for the current home
screen is roughly: three stat-bar setters, one text setter, two creature setters
(sprite + position), plus `on_frame`'s own interpretation — call it **20–40 µs
of binding overhead**, against `KF_FRAME_BUDGET_US` = 33,333.

**The expectation is that drawing gets cheaper, not more expensive.** Today
`kf_creature_screen_frame()` erases `g_previous` (2,304 opaque px ≈ 23 µs at
`KF_DRAW_OPAQUE_PX_PER_US` 100) and redraws the sprite (2,304 keyed px ≈ 92 µs
at `KF_DRAW_KEYED_PX_PER_US` 25) **unconditionally, every frame, whether or not
the creature moved** (`:1128`, `:1240-1242`). A retained frame in which the
creature did not move does neither.

How it is measured, on both targets:

- **Desktop:** `kf_app_last_frame()->{draw_us, keyed_pixels, opaque_pixels,
  dirty_rect_count}`, already exercised by `frame_counters_check`.
- **Device:** `python3 tools/kf_debug.py state --json` — `draw_us`, `keyed_px`,
  `opaque_px`, `dirty_rects`, `cpu_us`, **`post_us`**. `post_us` is the number
  to watch: `kf_lua_port_frame()` runs inside the bracket
  `app_main.cpp:307-331` measures, so Lua's own time lands there and nowhere
  else.

**Gates.** Task 5 records `post_us` before and after on the same board and the
same pet; a rise of more than **2,000 µs** (6% of the frame) is a finding to
investigate, not a number to accept. `keyed_px` on a moving frame stays in the
same low thousands it is in today. `dirty_rects` never exceeds 5 on the home
screen and never reaches 8.

**One number this plan does not guess at:** `KF_DRAW_KEYED_PX_PER_US = 25` is
flagged in `kf/budget.h` as "ASSUMPTION, NOT MEASURED". Nothing here depends on
it being right — every claim above is a comparison between two code paths
charged through the same constant.

---

## Risks

| # | Risk | Retired by | Cost if it bites |
|---|---|---|---|
| 1 | **A retained differ can be subtly wrong in a way a still screenshot does not show** — a stale rect, an object repainted in the wrong layer order, a rect that was needed and not marked. | Task 4 rebuilds the *existing* screen on the differ and puts it under ten passing tests, with no Lua involved. Task 5 diffs C++ against Lua frame by frame. | Visual corruption that only appears after a specific sequence of movements. The A/B diff is what makes it findable. |
| 2 | **A Lua script can hang the frame loop.** No `lua_sethook`, no deadline (ADR 0014:195, ADR 0028:190). | **Nothing in this plan.** Task 9, deliberately deferred. | Today: a hung log. After Task 6: a frozen device needing a power cycle. Acceptable while Chris is the only script author; not acceptable before third parties ship. |
| 3 | **Lua's 1 MB arena has to hold the scene binding's per-object userdata as well as the script.** | Task 3 reports `kf_lua_alloc_get_stats().live_bytes` in its check and asserts a ceiling. | Out-of-memory surfaces as a catchable *script* error (`kf_lua_alloc.cpp:253-276`), not a crash — so this degrades legibly, which is why it is a risk and not a blocker. |
| 4 | **`KF_ARENA_LUA` is in PSRAM and the ESP32 build uses `LUA_32BITS`.** A binding that compiles on desktop can fail to compile or silently narrow on device. | Every task below ends with an ESP-IDF cross-compile, not just a desktop build. | Found at the end of a task instead of the end of the plan. |
| 5 | **Rebuilding the screen changes what renders in a way nobody notices.** | Task 4 must pass all ten creature-screen checks **without editing their assertions**, except `frame_counters_check`, whose edit is specified and justified in advance. | An unnoticed visual regression shipped to hardware. |

---

## What lands tonight, and what does not

Honest split. The owner wants hardware-ready tomorrow.

**Tonight: Tasks 1, 2 and 3.** They are ordered cheapest-and-most-informative
first, and — this is the point — **none of them changes a single pixel of what
renders.** Task 1 is a file move plus a generator. Task 2 adds a new Core module
that nothing calls yet. Task 3 adds a Lua binding exercised only by a headless
check. The branch is green and shippable after each, the device build is
byte-for-byte equivalent in behaviour, and **redeploying to hardware tomorrow
carries no new risk from any of them.**

After tonight, a developer can read and edit `examples/creature_demo/creature.lua`
as a file, and a Lua script can put a sprite on the screen through a proved
retained-mode engine. That is the platform promise made real. What it is not yet
is *the demo pet running off it* — that is Tasks 4 through 6.

**Not tonight: Tasks 4–9.** Task 4 (rebuild the C++ screen on the scene) is the
one with real risk and it deserves a fresh session, because it is the task where
ten existing tests are the judge. Tasks 5 and 6 are the actual switchover. 7–9
are navigation, the Info screen, and cartridge loading.

**A migration that leaves the game half in each language across three tasks is a
bad plan**, and this sequence deliberately avoids it: at no point between Task 4
and Task 6 is the home screen partly C++ and partly Lua. Task 4 is
all-C++-on-the-new-engine; Task 5 is both, complete, side by side, compared;
Task 6 flips one default.

---

## Task sequence

Each task below is a heading and its requirements. Tasks 1–3 are specified to
step level. Tasks 4–9 are specified to requirement level, because their detail
depends on what Tasks 1–3 measure and writing it now would be inventing.

### Task 1: The game script becomes a file, and the SDK gets a home

**Why first:** it costs the least, it is the only task tonight that a
non-programmer would notice, and everything reviewable after it is reviewable as
Lua rather than as a C++ string literal.

**Files:**
- Create: `sdk/lua/` — move `simulator/src/lua/{kf_lua_port,kf_lua_alloc}.{h,cpp}` here
- Create: `examples/creature_demo/creature.lua` (the demo script, extracted verbatim)
- Create: `tools/kf_embed_lua.py` (`.lua` → a `const char *` header)
- Modify: `simulator/src/lua/kf_lua_demo_creature_script.h` → generated, moved to `sdk/lua/generated/`
- Modify: `simulator/CMakeLists.txt`, `ports/esp32/main/CMakeLists.txt` (paths)
- Modify: `sdk/lua/kf_lua_port.h` (the stale "simulator-only" comment, lines 9–12)
- Test: a new `lua_embed_check` that regenerates every embedded header and diffs

**Requirements:**

- [ ] `examples/creature_demo/creature.lua` carries an SPDX Apache-2.0 header and
      is **byte-identical in content** to the Lua currently inside
      `kKfLuaDemoCreatureScriptSource` (lines 48–138 of the existing header).
      Nothing about behaviour changes in this task.
- [ ] `tools/kf_embed_lua.py` takes a `.lua` path and emits the same header shape
      that exists today: an `inline constexpr const char *` with an `R"lua(...)lua"`
      body and a matching `...ChunkName` constant. It must **refuse** input
      containing the sequence `)lua"` with a message naming the line, since that
      would terminate the raw literal early and produce a compile error a hundred
      lines from the cause.
- [ ] The generated header is **checked in**. `lua_embed_check` regenerates into a
      temp dir and diffs; a mismatch fails with the command to fix it.
- [ ] The eight proof scripts in `kf_lua_pet_proof_script.h` and
      `kf_lua_proof_script.h` **stay as they are** in this task. They are test
      fixtures, not game code, and moving nine things at once when one is
      load-bearing is how a mechanical task becomes a debugging session. Note them
      as follow-up.
- [ ] Fix `kf_lua_port.h:9-12`. It claims Lua is not wired into `ports/esp32/`;
      `ports/esp32/main/CMakeLists.txt:96-97` and `app_main.cpp:216` say otherwise.
      Replace it with what is true, and name ADR 0028 as the decision that made it so.
- [ ] `ctest --test-dir build` → **41/41**. `python3 tools/check_no_heap.py .` →
      clean. ESP-IDF cross-compile clean, zero warnings, firmware size within a few
      hundred bytes of the last recorded figure.

**How you would know it worked:** edit one word in
`examples/creature_demo/creature.lua`, run `python3 tools/kf_embed_lua.py`,
rebuild, and see the new word in the simulator's console. If that loop does not
work, the task is not done.

---

### Task 2: `kf/scene.h` — the retained scene and the differ, in Core

No Lua. Nothing calls it yet. This is the engine, built and proved on its own.

**Files:**
- Create: `hakoniwaos/include/kf/scene.h`, `hakoniwaos/src/scene.cpp`
- Modify: `hakoniwaos/sources.cmake` — **one line, one place.** That file is the
  single source list both the host build and the ESP-IDF build read, and its own
  header comment explains it is "the mechanism that stops the two from drifting
  apart". Do not add the file to either `CMakeLists.txt` directly.
- Test: `simulator/src/headless/headless_main.cpp` — `run_scene_check()`,
  flag `--verify-scene`
- Modify: `simulator/CMakeLists.txt` (register `scene_check`)
- Create: `docs/architecture/adr-0040-retained-scene.md`

**The API surface this task produces.** Names Tasks 3–8 all depend on, so they
are stated exactly. Integer handles, not pointers — a handle survives a scene
reset as an obviously-invalid value, where a dangling pointer does not.

```c
typedef uint16_t kf_scene_id;          /* 0 is never a valid object */
#define KF_SCENE_MAX_OBJECTS 64
#define KF_SCENE_MAX_DIRTY_CANDIDATES 32

void        kf_scene_reset(void);
void        kf_scene_set_background_color(kf_color c);
void        kf_scene_set_background_sprite(const char *name);

kf_scene_id kf_scene_add_sprite(const char *name);
kf_scene_id kf_scene_add_text(const char *str);
kf_scene_id kf_scene_add_box(int16_t w, int16_t h, kf_color c);
void        kf_scene_remove(kf_scene_id id);

void        kf_scene_set_pos(kf_scene_id id, int16_t x, int16_t y);
void        kf_scene_set_visible(kf_scene_id id, bool visible);
void        kf_scene_set_layer(kf_scene_id id, int8_t layer);
void        kf_scene_set_sprite(kf_scene_id id, const char *name);
void        kf_scene_set_frame(kf_scene_id id, uint16_t frame);
void        kf_scene_set_mirrored(kf_scene_id id, bool mirrored);
void        kf_scene_set_text(kf_scene_id id, const char *str);
void        kf_scene_set_colors(kf_scene_id id, kf_color fg, kf_color bg);
void        kf_scene_set_size(kf_scene_id id, int16_t w, int16_t h);

kf_rect     kf_scene_bounds(kf_scene_id id);
void        kf_scene_commit(void);
```

**Requirements:**

- [ ] `hakoniwaos/src/scene.cpp` holds **two file-static arrays** — the objects
      and the dirty candidates — and allocates nothing. `check_no_heap.py` must stay
      clean. No `float`, no `double`.
- [ ] Strings are **copied into a fixed field**, not held by pointer. A sprite
      name is at most 31 characters plus NUL, matching the pack's own 32-byte name
      field (`hakoniwaos/src/assets.cpp`); text is capped at 40 characters, which is
      exactly one full 240px row at `KF_FONT_CELL_W` 6. A longer string is truncated
      and logged once naming the object, not silently cut. **A pointer would be the
      easy choice and it is wrong** — a Lua string is garbage-collected and Core
      would be dereferencing freed memory two frames later.
- [ ] `kf_scene_commit()` resolves sprite names through `kf_assets_get()` **once
      per name change**, caching the `const kf_sprite *` on the object — the same
      thing `g_sprite_cache` (`kf_creature_screen.cpp:393`) already does and for the
      same reason. A name that does not resolve caches `nullptr`, draws
      `KF_RGB(255,0,128)`, and logs once.
- [ ] Draw order is `layer` ascending, ties by creation order. Document that ties
      are stable, because a differ whose paint order can change between frames
      produces flicker nobody can reproduce.
- [ ] `run_scene_check()` must assert, in this order, and the first one is the
      most important:
      1. **A committed frame with no changes marks zero dirty rectangles and draws
         zero pixels.** `kf_draw_counters_get()` reports 0/0 and
         `kf_fb_dirty_rects().count == 0`.
      2. Moving one object marks a rect covering both its old and its new position
         (they merge when they overlap — `touches_or_overlaps()` expands by 1px,
         `framebuffer.cpp:43`).
      3. The framebuffer after a commit is `memcmp`-identical to the same scene
         drawn by hand with `kf_fill_rect` + `kf_blit_frame` in the same order.
         **This is the task's real proof** and it needs no golden constant.
      4. Declaring 12 independently-moving objects produces
         `kf_fb_dirty_rects().count <= KF_MAX_DIRTY_RECTS` and **does not collapse
         to full screen** — assert the total dirty area is well under
         `KF_FRAMEBUFFER_BYTES`, which is what proves the coalescer beat the
         framebuffer's own fallback.
      5. Anti-vacuity: the check must **fail** if `kf_scene_commit()`'s body is
         deleted. Verify that by actually deleting it once and watching it go red.
- [ ] `ctest --test-dir build` → **42/42**. Cross-compile clean.
- [ ] ADR 0040 records: retained over immediate and why (the 31 ms arithmetic);
      handles over pointers; strings copied not referenced; the coalescing rule and
      why it beats the framebuffer's collapse; the whole-object overdraw and the
      `kf_clip_*` follow-up that is deliberately not built; a "Not verified" section
      saying no scene has yet rendered on hardware.

---

### Task 3: The Lua binding over the scene

**Files:**
- Create: `sdk/lua/kf_lua_scene.cpp`, `sdk/lua/kf_lua_scene.h`
- Modify: `sdk/lua/kf_lua_port.cpp` (register the new `kf` entries; call the
  scene binding's init/shutdown)
- Modify: `simulator/CMakeLists.txt`, `ports/esp32/main/CMakeLists.txt`
- Create: `examples/hello_pet/pet.lua` (the minimal-pet listing above, verbatim)
- Test: `run_lua_draw_check()`, flag `--verify-lua-draw`
- Create: `docs/architecture/adr-0041-lua-drawing-binding.md`

**Requirements:**

- [ ] Implement the `kf.*` table entries and the object metatable exactly as
      specified in "The API, in Lua" above. **No-arg reads, args write**, on every
      property, with no exceptions — an exception is a rule the audience has to
      remember.
- [ ] Every numeric argument goes through `luaL_checkinteger`, never
      `luaL_checknumber`. Lua is built `LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` and, on the
      ESP32, `LUA_32BITS`; a `luaL_checknumber` would silently accept `96.7` and
      truncate it differently on the two targets.
- [ ] An object is a **userdata holding the `kf_scene_id`**, with a shared
      metatable. Not a table of closures — 64 objects times a dozen closures each is
      real pressure on a 1 MB arena for no benefit.
- [ ] `obj:remove()` calls `kf_scene_remove()` and marks the userdata dead. Every
      subsequent method on it raises a Lua error naming the object as removed,
      rather than silently operating on a slot something else now owns.
- [ ] **`kf.on_button` reads `kf_app_buttons_pressed()`** — the debounced edge
      mask from `hakoniwaos/src/app.cpp:496`, the same one `kf_screen_nav_frame()`
      and `handle_care_buttons()` already read. This is what keeps `KFDBG BTN` and
      `KFDBG BTNHOLD` working, and it is a requirement, not an implementation
      detail. Callbacks fire from inside `kf_lua_port_frame()`, before `on_frame`,
      each in its own `lua_pcall` so one bad handler does not take down the frame.
- [ ] **`kf_scene_commit()` is called by the frame loop, not by the binding.**
      Add the call to `sdl_main.cpp` and `app_main.cpp` immediately after
      `kf_lua_port_frame(0)`, which is already the last thing in both loop bodies
      (`sdl_main.cpp:246`, `app_main.cpp:329`) and therefore already in the right
      place — present happens at the top of the *next* `kf_app_frame()`, so a scene
      committed here reaches the panel on the following frame, exactly as the
      creature screen's drawing already does.
- [ ] **Nothing interactive declares a scene in this task.** The demo creature
      script still only logs, so `kf_scene_commit()` runs against an empty scene and
      draws nothing. That is what keeps the branch shippable and the device
      unchanged tonight. Add an explicit early-out and a comment saying so.
- [ ] `run_lua_draw_check()` loads a small inline script that declares a
      background and one sprite at a known position, commits, and asserts the
      framebuffer is `memcmp`-identical to the same thing drawn with `kf_fill` +
      `kf_blit_frame` directly. Then it asserts a second commit with no script
      changes draws **zero** pixels. Then it asserts a bad sprite name draws the
      magenta placeholder rather than nothing.
- [ ] The check also reports `kf_lua_alloc_get_stats().live_bytes` and asserts it
      stays under **256 KB** with 64 objects live — a quarter of the arena, leaving
      the script itself three quarters.
- [ ] `ctest --test-dir build` → **43/43**. Cross-compile clean. Record the
      firmware size delta; Lua bindings are cheap but this is the first one that
      adds a metatable and it should be a few KB, not tens.
- [ ] ADR 0041 records the accessor convention and why (the jQuery idiom), the
      userdata choice, the uppercasing rule and its justification, the error
      behaviours from the table above, and a "Not verified" section: no Lua-declared
      scene has rendered on hardware.

---

### Task 4: The C++ creature screen is rebuilt on the scene — no Lua involved

The task with real risk, and the reason it has none of the Lua in it.
`kf_creature_screen.cpp` keeps its 3 public and 8 debug entry points **exactly
as declared today**; its 1,364 lines of imperative drawing become scene
declarations. Nine of the ten creature-screen checks must pass with their
assertions untouched.

**Requirements, stated so the task can be judged:**

- [ ] The eleven `g_*` file-statics listed in `kf_creature_screen.cpp` collapse
      into a handful of `kf_scene_id`s plus the pet-facing state that is genuinely
      not presentation (`g_creature`, `g_feed_variation` and its three siblings).
      `g_previous`, `g_drawn_poops`, `g_drawn_stat_px` and `g_sprite_cache` all
      **disappear** — they are hand-rolled differ state and the differ now owns it.
      That is most of the 1,364 lines.
- [ ] `kf_creature_screen_enter()` becomes `kf_scene_reset()` plus the
      declarations; `kf_creature_screen_frame()` becomes the per-frame setters.
- [ ] All eight `kf_creature_screen_debug_*` accessors keep their exact
      signatures and semantics. `_debug_bounds()` and `_debug_stat_bar_bounds()`
      become `kf_scene_bounds()` lookups; `_debug_stat_bar_filled_px()` reads the
      declared box width.
- [ ] **`run_frame_counters_check()` is edited, and only in this way:** it must
      drive a frame on which the creature actually moves, so `keyed_pixels` is
      genuinely non-zero, rather than relying on the old unconditional redraw. Do
      **not** widen the `[2000, 8000]` window to include zero. A check that passes
      with the drawing deleted proves nothing, and this codebase has been bitten by
      exactly that twice (`2026-08-09-creature-on-screen.md`).
- [ ] The other nine checks pass **without editing an assertion**. Their
      dirty-rect bounds are `<=`, and retained mode makes those counts fall, so they
      should hold with room to spare. If one fails, the differ is wrong — that is
      the whole reason this task exists before any Lua.
- [ ] Record the measured before/after for `keyed_pixels`, `opaque_pixels` and
      `dirty_rect_count` on a still frame and a moving frame. The still-frame
      numbers should go to zero. That is the performance claim, made as a
      measurement.
- [ ] Cross-compile, flash, and confirm on the board with
      `python3 tools/kf_debug.py state --json` that `keyed_px` is zero on a still
      frame and low thousands on a moving one, and that `post_us` has not risen.

---

### Task 5: The demo creature declares the whole home screen, and the two are diffed

Both screens exist. A build flag chooses; the default stays C++.

**Requirements:**

- [ ] `examples/creature_demo/creature.lua` grows from "observe and narrate" to
      "declare the home screen": background, creature sprite with pose and facing,
      the mess, the three stat bars and their labels, the care guide. The narration
      stays — it is good, and it now sits next to drawing code in the same file,
      which is the worked example the SDK needs.
- [ ] A CMake option `KF_HOME_SCREEN` accepting `cpp` (default) or `lua`, on both
      build systems, validated at configure time with `FATAL_ERROR` on anything
      else.
- [ ] **`run_lua_vs_cpp_screen_check()`**, flag `--verify-screen-parity`: seed a
      pet, run N frames through the C++ screen accumulating the same FNV-1a rolling
      hash `headless_display.cpp:54-78` already computes, reset, run the identical
      N frames through the Lua screen, and compare. **Per-frame**, not just at the
      end, so a divergence names the frame it started on. This is the correctness
      argument for the whole plan.
- [ ] Where the two legitimately differ — and something will, most likely the
      exact pixel a text baseline lands on — the *Lua* one is changed to match, and
      the difference is recorded. Do not relax the comparison to a tolerance. A
      tolerance here is a place for a real bug to hide.
- [ ] A one-line error banner: when `on_frame` has been disabled by an error, the
      engine draws the message into the reserved band. The panel says what the
      console says.
- [ ] On hardware: flash both builds, `KFDBG SHOT` each, compare the PNGs. Record
      `post_us` for both.

### Task 6: The default flips, and the wander moves to Lua

- [x] `KF_HOME_SCREEN` defaults to `lua`. `cpp` stays buildable for one release
      as the fallback, and `--verify-screen-parity` stays in CI as the thing that
      keeps them honest. **Done** — also required a new `kf/scene.h` primitive
      (`kf_scene_force_repaint()`, ADR 0043) the plan's own end note already
      flagged as a prerequisite; see that ADR for the repaint fix and why
      `screen_nav_check` needed more than the primitive alone.
- [ ] `hakoniwaos/src/creature.cpp`'s wander (`kf_creature_update`,
      `choose_target`, `kSpeedPxPerSec`, the dwell) moves into the Lua script.
      `kf_creature_pose_for()` and `kf_creature_sprite_name()` move too — they are
      the game's vocabulary, not the OS's. `kf_anim` and `kf_creature_tick_anim()`
      **stay in Core**: the animation clock is engine, per the table.
      **Deliberately NOT done** — ADR 0043's "What did NOT move" records why:
      keeping `run_lua_vs_cpp_screen_check()` meaningful across this move needs a
      Lua reimplementation that draws from `kf/rng.h` in bit-exact lockstep with
      the C++ path, real separate work with its own real risk, not a natural
      rider on the default flip. Still open, most naturally alongside Task 7.
- [ ] Parity holds across the move, or the move is wrong. **N/A this task** — no
      move happened; `run_lua_vs_cpp_screen_check()` continues to pass because
      both screens still share the one C++ wander exactly as Task 5 left it.

### Task 7: Input and navigation are the game's

- [ ] `kf.screen(name)` creates a named scene; `screen:show()` switches.
- [ ] **`kf_screen_nav_debug_advance()` / `_home()` / `_debug_index()` keep
      working**, because `sdl_debug_window.cpp:444` and `:871` call them. Keep
      `kf_screen_nav.cpp` as the C++-side registry of which scene is showing, with
      Lua registering into it. This is the one debug coupling in the whole plan that
      genuinely needs rework, and it is one function's worth.

### Task 8: The Info screen leaves LVGL

- [ ] `kf_pet_info_screen.cpp`'s widget tree becomes a Lua scene.
      `screen_nav_check`'s golden constant `ac44bb9819809bea` moves legitimately
      here; re-baseline it **with the before/after screenshots recorded in the
      task's ADR**, which is the only circumstance in which this project moves a
      golden constant.
- [ ] With both screens off LVGL, `kf_screen_nav_wants_lvgl()` always returns
      false and LVGL becomes optional. Whether to drop it is a separate decision
      (`CLAUDE.md`: "LVGL vs a custom sprite engine" is a deliberate later
      evaluation) — **do not make it here.**

### Task 9: Cartridges, and a script that cannot hang the device

- [ ] A `.lua` file travels in the asset pack, so changing the game does not mean
      reflashing firmware. `tools/kf_pack_assets.py` gains a script asset type; the
      loader reads it through the mapped pointer the same way sprites are read.
- [ ] `lua_sethook` with `LUA_MASKCOUNT`, budgeted against `KF_FRAME_BUDGET_US`,
      so a runaway script yields with a legible error instead of hanging the loop.
      Retires risk 2.
- [ ] The packaging CLI `CLAUDE.md` promises under `/sdk`.

---

## Licensing: how the example stays copyable while the art stays Chris's

`LICENSING.md` splits the demo creature: **code Apache 2.0, characters and
artwork all rights reserved.** Once the demo script is the thing every third-
party developer copies from, that split has to be visible in the file layout,
not just in a markdown page.

The mechanism, and it is a real one rather than a notice:

1. **The script never contains art.** It references pack entries by name —
   `"baby_neutral_s"` — and names are functional identifiers, not artwork. A
   developer copying `creature.lua` copies logic and gets no pixels.
2. **The script runs against any pack that provides the same entry names.** That
   is the testable form of the promise, and it is worth testing: a check that
   runs the demo script against a placeholder pack (generated by
   `tools/kf_pack_assets.py --test-sprite`, no copyrighted art in it) and asserts
   it still renders. If that check passes, "copy the code, bring your own art"
   is a fact about the repository rather than a claim in a document.
   **Add it in Task 5**, where the script first draws anything.
3. **The two live in different files with different headers.**
   `examples/creature_demo/creature.lua` carries `SPDX-License-Identifier:
   Apache-2.0`. `examples/creature_demo/assets.kfpack` and the source art get a
   sibling `examples/creature_demo/ART-LICENSE.md` stating all rights reserved
   and pointing at `LICENSING.md`. Today the pack sits in an Apache-2.0 tree with
   nothing next to it saying otherwise, which is the weakest point in the split
   and costs one file to fix. **Task 1 adds it**, since Task 1 is already
   rearranging that directory.
4. **The script invents no names.** `docs/sdk-style-guide.md:55-64` already draws
   this line and `pet.teen_form()` already returns a raw index. The drawing
   bindings must hold it too: `kf.sprite()` takes whatever name the pack has, and
   Core never invents one.

The result a third-party developer sees: open `creature.lua`, read it, copy it,
point it at their own `.kfpack`, ship it commercially, owe nothing. Which is
what `LICENSING.md` promises and what the platform is for.

---

## What this plan deliberately does not do

Each of these is separable, and most are named here specifically so nobody
mistakes them for oversights.

- **It does not move the pet simulation into Lua.** `CLAUDE.md` puts the pet
  framework in `/hakoniwaos` on purpose: HakoniwaOS *provides* a pet. Whether a
  third-party creature should define its own needs, decay curves and evolution
  rules is a genuine, open question — and answering it in the same plan that
  moves the renderer would mean any odd behaviour has two candidate causes and
  no way to separate them. It is also the change that would break the SDL debug
  window, `KFDBG ADVANCE/RESET/JUMP/FEED/...` and `kf_panel.py` all at once,
  none of which this plan touches. Its own plan, later.

- **It does not add a Lua execution-time limit.** Risk 2, Task 9. A `while true
  do end` hangs the frame loop today and will still hang it after Task 6. That
  is acceptable while Chris is the only script author and unacceptable before
  anyone else ships one, which is exactly when Task 9 lands.

- **It does not decide LVGL's fate.** Task 8 makes LVGL optional. Whether to
  remove it is the deliberate later evaluation `CLAUDE.md` names, and this plan
  must not prejudge it by deleting the dependency as a side effect of tidying.

- **It does not answer the `1:FEED` question.** The care guide names keyboard
  keys the device does not have;
  `2026-08-11-hardware-bringup.md:197-224` lays out four options and their costs
  and says it is Chris's call. Moving the guide into Lua makes the change
  *cheaper* to apply — one line in a script rather than a rebuild — but the plan
  ships whatever labels are in the tree.

- **It does not add a KFDBG transport for the desktop simulator.**
  `tools/kf_panel.py:148` still raises on `sim:` targets. One host tool driving
  both builds is exactly this project's architecture and it would be genuinely
  useful, but nothing here needs it and the desktop already has a debug window
  that does more.

- **It does not move the eight Lua proof scripts to files.** Task 1 moves the one
  that is game code. The proof scripts are test fixtures, and mechanically moving
  nine things when one is load-bearing is how a safe task becomes a debugging
  session. Follow-up, cheap, whenever.

- **It does not generate any art.** Every shipped sprite outside the egg and the
  five idle poses is still single-frame
  (`2026-08-10-animated-indexed-sprites.md`), and `KF_CREATURE_POSE_SLEEPING` is
  still unreachable because Core has no sleep field. Neither changes anything
  here: the scene plays whatever frames a sprite has.

- **It does not add a clipped blit.** `kf_clip_push()` / `kf_clip_pop()` is the
  clean fix for the whole-object overdraw described under "Coalescing". It would
  touch every blit call site to buy something nothing is currently short of.
  Named so the next reader knows it was considered, not missed.

---

## Status: Tasks 1-6 COMPLETE (wander migration deferred), 2026-08-12

| Task | Commits | Result |
|---|---|---|
| 1 — real `.lua` file, generator, `sdk/` | `8155af5`, `9bf948f` | `examples/creature_demo/creature.lua` exists; drift test guards the generated header |
| 2 — the retained scene differ, in Core | `bc1f938`, `9f7e81f`, `b25692a` | `kf/scene.h`; 12 moving objects coalesce to 8 rects / 11,200 of 153,600 bytes; ADR 0040 |
| 3 — the Lua drawing binding | `76e087b`, `b427335`, `58deb89`, `1905f11` | `sdk/lua/kf_lua_scene.*`; `examples/hello_pet/pet.lua`; ADR 0041 |
| 4 — the C++ screen rebuilt on the scene | `df5e9fc`, `0b6f8f4`, `8b1ee58`, `d533bd9`, `58492b1`, `3f9ab61` | Ten creature-screen tests pass unchanged; eight discrete poop objects; ADR 0040 opportunistic-merge addendum |
| 5 — the demo creature declares the whole home screen | see `.superpowers/sdd/lua-task-5-report.md` | `KF_HOME_SCREEN=cpp\|lua`; `run_lua_vs_cpp_screen_check()` proves 250 frames byte-identical; `kf_creature_presenter.h`/`kf_home_screen_input.h` split out so both screens share one wander and one set of buttons; ADR 0042 |
| 6 — repaint capability + the default flips to `lua` | see `.superpowers/sdd/lua-task-6-report.md` | `kf_scene_force_repaint()` (ADR 0043) closes ADR 0042's known gap; `screen_nav_check` passes under `lua`; `KF_HOME_SCREEN` defaults to `lua` on both build systems; the wander migration named in this task's own "What moves and what stays" table was deliberately **not** done — see ADR 0043's "What did NOT move" |

**44/44 on the default build (now `lua`) as of Task 6's completion,
2026-08-12 — re-run `ctest --test-dir build -N` for today's count, since
later work adds to it. Core heap-free, ESP32 firmware
~672KB (57% of partition free) in BOTH `KF_HOME_SCREEN` values, verified as
a genuinely fresh-configured default (`cmake -UKF_HOME_SCREEN`), not just an
explicit override. Both golden rendering checksums unmoved. The dirty-rect
worst case is unchanged at 3 of 8.** The demo creature draws Home by
default now, Lua declaring every object every frame, with `cpp` kept
building and passing under `KF_HOME_SCREEN=cpp` as the parity reference and
fallback. See the Task 6 report for what `screen_nav_check` needed beyond
the repaint primitive itself (it had never booted the Lua VM at all) and
the reasoning for deferring the wander move.

**Resume at Task 7 (or a Task 6b scoped to just the wander).** Genuinely
moving `kf_creature_update`/`choose_target`/pose selection into
`creature.lua`, matching `hakoniwaos/src/creature.cpp`'s fixed-point
arithmetic and `kf/rng.h` draw order bit-for-bit so
`run_lua_vs_cpp_screen_check()` stays meaningful, is still open — ADR 0043
records why it was not attempted alongside the default flip. Task 7's
`kf.screen()`/`screen:show()` (named-scene navigation) is unrelated to this
and still ready to start independently — `--verify-screen-parity` stays in
CI as the thing that keeps whichever comes first honest.

### The API that came out of it

Task 5 additions marked `[T5]` — everything else is Task 3.

```
kf.sprite(name) · kf.text(str) · kf.box(w,h,color) · kf.background(color_or_name)
kf.color(r,g,b) · kf.WHITE/BLACK/RED/... · kf.on_button(name,fn)
kf.width() · kf.height() · kf.sprites()
kf.home_screen_active()                 -- [T5] read-only; see ADR 0042

obj:move(x,y) · obj:x([n]) · obj:y([n]) · obj:show() · obj:hide()
obj:visible([bool]) · obj:layer([n]) · obj:remove()
obj:sprite([name]) · obj:flip([bool])   -- sprite only
obj:frame([n])                          -- [T5] sprite only -- kf_scene_set_frame() existed
                                         --      since Task 2, Task 3 never bound it
obj:set([str])                          -- text only
obj:color([fg[,bg]])                    -- text or box
obj:size([w,h])                         -- box only

creature.x() · creature.y() · creature.sprite() · creature.mirrored() · creature.frame()
                                         -- [T5] read-only; the wander stays in C++
                                         --      until Task 6 -- see ADR 0042
```

No-arg reads, args write. No dirty rectangles, frame indices, colour keys, byte
order or memory management anywhere in the surface — all of that stayed in Core,
which was the whole point of retained mode.

### What a script can and cannot do to the device

**Cannot crash it.** Unknown sprite names draw the magenta placeholder and log
by name. Scene overflow, wrong-kind calls and use-after-remove all raise named
Lua errors, caught by `kf_lua_port_frame()`'s existing `lua_pcall` — `on_frame`
is disabled until re-init and the last good frame stays on screen. Button
handlers each run in their own `pcall`. Numeric arguments are clamped, never
wrapped.

**Can still hang it.** `while true do end` freezes the frame loop; there is no
execution-time guard. Deferred to Task 9 and it should not slip — it stops being
theoretical the moment Lua owns the screen.

### Found in passing, worth fixing before it spreads

`g_objects[64]` in Task 2's `scene.cpp` measures **224 bytes per object — 5.6x
ADR 0040's own estimate — and lands in `.data` rather than `.bss`**, so it costs
both flash and RAM, because `RenderState::fg` defaults to a non-zero
`KF_WHITE`. Task 3 flagged it rather than reaching into Task 2's file. A
zero-default with the white applied at draw time would move the whole array back
to `.bss`.

Also open: `kf.on_button` has no dedicated headless check. It was code-reviewed
against `kf_screen_nav_frame()`'s identical read, and adding one risks moving
the golden checksums — the same tradeoff `run_creature_screen_input_check()`
already made.

---

## Comments in Lua cost flash and boot time — found during Task 1

This project's house style is unusually thorough comments explaining *why*, and
that has been free everywhere until now: a C++ comment vanishes at compile time.

**A Lua comment does not.** The script is embedded verbatim, ships to the device
inside the firmware image, and is parsed by the Lua VM at every boot.

Task 1's implementer wrote a full essay header on `creature.lua`, ported from
the C++ prose it replaced, and it put **~3.4 KB of comment text into flash**.
Trimming it to ~550 bytes is the difference between a +3.5 KB and a +544 byte
firmware delta.

**So, for `.lua` files specifically:**

- Explain *why* where the reasoning is load-bearing, in as few words as it takes.
- Put the long-form reasoning in the C++ binding, an ADR, or the SDK docs — all
  of which are free — and point at it from the script.
- Remember the demo script is also the reference other developers copy. Prose
  that teaches earns its bytes; prose that restates the code does not.

This does **not** relax the rule for C++ or Python in this repo. It is a real
constraint that applies only to text the device parses at runtime, and it will
apply to every game cartridge shipped on this platform.

---

## Task order changed: 7's repaint prerequisite comes before 6's flip

Task 5 found that `kf/scene.h` has **no way to force a full repaint without
destroying every object's identity**. That breaks Home->Info->Home navigation
under `KF_HOME_SCREEN=lua` — confirmed by `screen_nav_check` failing in that
build — because returning to Home needs the whole panel repainted, and the only
way to get one today is to tear down the scene, which loses the per-object
identity the differ needs to know what changed.

It documented this rather than patching it, correctly, and assigned it to
Task 7.

**Consequence: flipping the default (Task 6) on top of that ships broken
navigation.** So the repaint capability moves ahead of the flip. The two are
being done as one unit, because the flip cannot be verified without the
repaint and the repaint has no visible effect until the flip.

Success is not "the flag defaults to lua" — it is: the flag defaults to lua,
`screen_nav_check` passes, the parity check still holds, the ESP32 target
builds, and the owner can flash it and see the pet animating with Lua driving
the screen.
