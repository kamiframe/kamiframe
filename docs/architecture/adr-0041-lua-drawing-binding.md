# ADR 0041: The Lua binding over the retained scene

**Status:** Accepted
**Date:** 2026-08-12

## Context

Task 2 (ADR 0040) gave Core a retained scene and a coalescing differ,
proved with no Lua involved — `kf_scene_commit()` had exactly one caller,
`run_scene_check()`. Task 3 is the binding that lets a Lua script declare
into that scene: `kf.sprite`/`text`/`box`/`background`, the object methods,
and the frame-loop wiring that calls `kf_scene_commit()` once per frame.

The audience constraint (`CLAUDE.md`) is a hard requirement, restated
because every decision below serves it directly: *"a WordPress or jQuery
developer should not have too much trouble getting a pet running."*

## Decision

### The jQuery accessor convention: no-arg reads, args write

`body:x()` returns the x; `body:x(96)` sets it. One rule, applied to every
property of every object kind, with no exceptions. The alternative —
`get_x()`/`set_x()` pairs, or a property-bag table — is two things to
remember per property instead of one, and this audience already has the
jQuery idiom in their fingers (`$el.text()` / `$el.text("hi")`). `kf.sprite
(name)` and `obj:sprite(name)` share the verb deliberately, for the same
reason `$.ajax` and `$el.attr` share theirs: the receiver already
disambiguates them.

**This forced a decision Task 2 did not need to make.** Core's own API
(`kf/scene.h`) is write-only past `kf_scene_bounds()` — a rectangle, not the
individual fields. There is no `kf_scene_get_pos()` / `_get_layer()` /
`_get_text()`. The no-arg-read half of the convention above therefore has
nowhere to read *from* except a copy this binding keeps itself. Every
`LuaSceneObject` userdata (`kf_lua_scene.cpp`) mirrors, kind-agnostic,
everything Core's own `RenderState` holds — position, visibility, layer,
and every kind-specific field — and every setter writes both the shadow
copy and calls into Core. This is not a synchronization risk the way two
independently-mutable copies would be: the binding is the *only* writer of
scene state a script can reach, so its shadow copy and Core's declared
state can never disagree.

### Userdata holding an id, one shared metatable — not a table of closures

An object is a **full userdata** (`lua_newuserdatauv`) holding a
`LuaSceneObject` — the `kf_scene_id`, a kind tag, a removed flag, and the
shadow state above — against one metatable, `"kf.SceneObject"`, shared by
every object regardless of kind. The alternative the plan named and
rejected: a table of closures, one per method, captured over the object's
id. 64 objects times roughly a dozen closures each is real pressure on a 1
MB arena (`KF_ARENA_LUA_BYTES`) for something a shared metatable gets for
free — every object of every kind points at the same twelve function
values, never allocated per object.

Kind-specific methods (`:sprite`, `:flip`, `:set`, `:color`, `:size`) all
live on the one shared metatable rather than three separate ones, and check
`obj->kind` at the top, raising a named error on a mismatch (`':size' is
only valid on a box object`). Three metatables would mean picking one at
creation time based on which `kf.*` constructor was called — no simpler
than the single check each method already does, and it would have made the
"same word, different receiver" framing above (`kf.sprite`/`obj:sprite`)
harder to explain, not easier.

**Placement new, not a bare struct assignment.** `lua_newuserdatauv` hands
back memory Lua's allocator owns but has not constructed anything in.
`push_new_object()` calls `new (raw) LuaSceneObject()` rather than
assigning a temporary over the raw bytes — the correct way to begin a C++
object's lifetime in borrowed memory, not merely a style preference. No
`__gc` metamethod is registered: nothing a `LuaSceneObject` holds is a
resource outside its own inline fixed-size arrays, so there is nothing to
release, and Lua's GC freeing the raw bytes is already correct.

### `luaL_checkinteger`, never `luaL_checknumber`, on every numeric argument

Lua is built `LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` and, on the ESP32,
`LUA_32BITS`. `luaL_checknumber` would silently accept `body:move(96.7,
10)` and truncate the `.7` differently on the two targets — a script that
behaves one way on desktop and a different way on device, with nothing in
the script itself explaining why. Every numeric argument in this file goes
through `luaL_checkinteger` instead, which rejects a non-integer float
outright with Lua's own "number has no integer representation" error,
naming the script line.

Every value that crosses into a narrower C type (`int16_t` for position and
size, `int8_t` for layer, `kf_color`'s `uint16_t`) is then **clamped, not
wrapped** — `clamp_i16`, `clamp_i8`, `clamp_color`, `clamp_u8` in
`kf_lua_scene.cpp`. `lua_Integer` on desktop is 63 bits wide; a script that
passes `body:move(999999999, 0)` gets a sprite pinned at the panel's edge,
not one that silently integer-overflowed to a small negative x and
vanished off-screen in a way nothing in the script would explain.

### Removed objects: a named error, not Core's silent no-op

`kf_scene_remove()`'s own contract (`kf/scene.h`) is that every setter
becomes a safe no-op on an id that is not found or already removed — the
right behaviour for *Core*, because a stray call landing on a slot
something else now owns must never corrupt that new object.

This binding does not inherit that behaviour for its own methods. Every
method except `:remove()` itself routes through `check_live_obj()`, which
raises `"this object was already removed with :remove()"` if the userdata's
own `removed` flag is set. The two situations are different: Core's silent
no-op protects an object *the removed call did not create* from a stray
setter aimed at a slot that has since been reused. A script calling
`:move()` on a handle **it removed itself** is a script bug, and
`CLAUDE.md`'s own rule — *"a mistyped sprite name should say so, by name,
once"* — generalises past sprite names to every mistake this binding can
see coming. Silently doing nothing here would mean a script's sprite
stopped responding to input with no error anywhere to explain why.

`:remove()` itself is idempotent and never raises: it checks the flag
before calling `kf_scene_remove()` and returns quietly if already set,
matching Core's own "safe to call twice" contract for the one method where
calling it twice is a reasonable thing for a script to do (e.g. an
unconditional cleanup at the top of a screen-teardown function).

### `kf.on_button` reads the same debounced edge mask KFDBG already does

`kf_lua_scene_dispatch_buttons()` reads `kf_app_buttons_pressed()`
(`hakoniwaos/src/app.cpp:496`) — the identical debounced press-edge mask
`kf_screen_nav_frame()` and the creature screen's `handle_care_buttons()`
already read, not a second, independently-debounced input path. This is
what keeps `KFDBG BTN` and `KFDBG BTNHOLD` exercising the Lua binding too:
an injected button press over the debug bridge flows through
`kf_input_poll()` → `debounce()` → `kf_app_buttons_pressed()`, the same
value this binding reads.

One Lua function reference per button (`g_button_ref[KF_BUTTON_COUNT]`,
`luaL_ref`/`luaL_unref` into the registry), not a list — calling
`kf.on_button("a", fn)` twice replaces the handler rather than accumulating
a second one, matching the accessor convention's spirit of one call, one
current state. Each handler fires in its own `lua_pcall`, before `on_frame`
— a crashing handler is logged and skipped, the remaining handlers and
`on_frame` still run this frame, and a button press is visible to whatever
`on_frame` reads this same frame rather than one frame later.

### Uppercase text, and why the warning is per-character, not per-string

`kf/font.h`'s glyph set is space, `0`-`9`, `A`-`Z`, and
`". , : - / % + ( )"` — uppercase only. A script author who never learns
this sees their own text render as a row of blank cells: exactly the
silent failure `CLAUDE.md` forbids. So `kf.text()` and `:set()` uppercase
every ASCII letter before it reaches Core, and log once — not once per
frame, not once per string — for any character the font genuinely has no
glyph for, naming the byte. `g_warned_char[256]`, indexed by character
value, is process-lifetime: a label that redraws every frame with the same
unsupported character does not spam the log 30 times a second, and a
different unsupported character still gets its own, separate warning.

**The uppercasing working buffer is deliberately larger than
`KF_SCENE_TEXT_MAX`.** `kf_scene_add_text()` / `kf_scene_set_text()` do
their own length truncation and their own "too long" log
(`hakoniwaos/src/scene.cpp`'s `copy_truncated()`). Pre-truncating to
`KF_SCENE_TEXT_MAX` (40) in this file *before* handing the string to Core
would mean Core never sees a string longer than its own limit, so its
length warning would never fire — one binding's "helpfulness" silently
disabling another module's diagnostic. `kf_lua_scene.cpp` uppercases into a
256-byte working buffer and passes *that* to Core, so Core's own
truncation and its own log line still happen exactly as they would for a
C++ caller. Sprite names are never uppercased — only `kf.text()`/`:set()`
strings — because pack entry names are lowercase by convention
(`egg_idle_s`) and are not font glyphs at all.

### The guard that keeps this task from painting a frame black

`kf_scene_commit()`'s own contract (`kf/scene.h`, and the differ this
task's brief) is: call it once per frame, from the frame loop, not the
binding. `sdl_main.cpp` and `app_main.cpp` both add that call immediately
after `kf_lua_port_frame(0)`.

**Calling it unconditionally would have broken the task's own promise that
nothing interactive changes a single rendered pixel tonight.**
`hakoniwaos/src/scene.cpp`'s `g_force_full_redraw` starts `true` and stays
`true` until the process's *first* `kf_scene_commit()` call ever runs — by
design, so that first commit repaints correctly with no reset needed
(`kf_scene_reset()`'s own header comment). Nothing in this task, or in
either interactive loop, ever calls `kf_scene_reset()` — that is Task 4's
job. So the very first frame after boot, calling `kf_scene_commit()`
unconditionally would paint one solid `KF_BLACK` frame over whatever the
creature screen or LVGL had just drawn: a real, visible flash, not a
hypothetical one, and exactly the kind of thing this project's "does not
change a single pixel" claims must survive contact with.

`kf_lua_scene_declared_anything()` — a file-static flag, sticky for the
life of one `kf_lua_port_init()`/`_shutdown()` pair, set the moment
`kf.sprite`/`text`/`box`/`background` is first called — is the guard. Both
loops call `kf_scene_commit()` only when it is true. The demo creature
script (`examples/creature_demo/creature.lua`) still only logs, so the
flag never flips this task, and `kf_scene_commit()` is never called
device-side or in the SDL build — verified, not merely argued: see "The
proof" below.

## The proof

`run_lua_draw_check()` (`simulator/src/headless/headless_main.cpp`, flag
`--verify-lua-draw`) asserts, in order:

1. A background and one sprite, declared from a Lua script and committed,
   is `memcmp`-identical to the same picture drawn by hand with
   `kf_fill_rect()`/`kf_blit_frame()` — the same proof `run_scene_check()`
   makes for Core alone, with a script now in the loop.
2. A second commit with no script changes draws zero pixels and marks zero
   dirty rectangles.
3. A bad sprite name (`kf.sprite("does_not_exist_in_the_pack")`) draws the
   magenta placeholder at the declared position, checked by sampling the
   actual pixel, not merely asserting that Core logged an error.
4. A script holding 64 live objects (`KF_SCENE_MAX_OBJECTS`) keeps
   `kf_lua_alloc_get_stats().live_bytes` under 256KB — measured at 29,232
   bytes, well inside the ceiling.
5. **Anti-vacuity, the 65th object.** A script creating 65 objects fails to
   load — `kf_lua_port_init()` itself returns `false` — because the 65th
   `kf.sprite()` call raises a Lua error naming `KF_SCENE_MAX_OBJECTS`.
6. **Anti-vacuity, a removed object.** A script removes an object, then
   calls `:move()` on it inside its own `pcall`, and reports whether that
   call succeeded through `kf.report()`. The check asserts the report is 0
   — the `pcall` caught this binding's own error, not some unrelated
   failure.
7. **The minimal-pet example, read from disk and run verbatim.** The plan's
   own acceptance test: *"if it does not work verbatim when you are done,
   one of the two is wrong."* `run_lua_draw_check()` opens
   `examples/hello_pet/pet.lua` at run time (not a copy pasted into the
   `.cpp`), brings up a real pet session, and runs it for five frames,
   asserting all five completed with no script error. This is the only
   check in this file that exercises `pet.feed()`/`pet.play()`/`pet.stage()`
   /`pet.hunger()` alongside the drawing surface.

**Every anti-vacuity claim above was verified by hand, not merely
asserted.** `obj_move`'s call to `kf_scene_set_pos()` was temporarily
deleted, the suite was rebuilt and run, and checks 1 and 3 both went red —
the memcmp proof and the placeholder check, exactly the two a broken
`:move()` should break — then the change was reverted and the suite
confirmed green again. Checks 5 and 6 are themselves the anti-vacuity
proof for the overflow and removed-object *behaviours* (an accidentally
permissive limit or an accidentally-working removed handle would fail
them directly), so no separate deletion-and-revert was needed for those two.

`ctest --test-dir build` is 43/43 (baseline 42, this task adds
`lua_draw_check`). `python3 tools/check_no_heap.py .` stays clean — this
binding lives in `sdk/lua/`, outside the scanned directories, and adds
nothing to `hakoniwaos/` except two small, heap-free enumeration functions
(`kf_assets_count()`/`kf_assets_name_at()`, needed for `kf.sprites()` — not
in Task 2's original API surface, added here because nothing else in Core
could answer "what sprite names does the mounted pack have"). The ESP-IDF
cross-compile (`-DKF_PANEL=ili9341`) is clean.

## Firmware size

Measured with `idf.py size-components`, comparing this task's commit
against the immediately preceding one (Task 2, scene engine committed but
never linked in — nothing called it):

| Archive | Before | After | Delta |
|---|---|---|---|
| `libmain.a` (the Lua binding itself, plus trivial call-site changes in `app_main.cpp`) | 18,533 | 21,725 | **+3,192** (+2,659 code, +248 rodata, +285 bss) |
| `libhakoniwaos.a` (Core) | 16,966 | 34,626 | **+17,660** |
| Whole firmware image | 662,048 | 683,792 | **+21,744** |

**The Lua binding's own cost is +3,192 bytes — a few KB, as expected, not
tens.** The much larger `libhakoniwaos.a` jump is not this task's binding
code: it is `hakoniwaos/src/scene.cpp` being linked into the ESP32 image
for the first time. Task 2 committed that file, but nothing called any
`kf_scene_*` function yet, so the linker discarded the whole object file —
this task is the first caller, so it is the first build where the cost
becomes real. `+2,937` of that jump is the differ/coalescer's own compiled
code (`kf_scene_commit()` and its helpers) — legitimately "a few KB" by the
same standard. `+14,336` bytes is `g_objects[KF_SCENE_MAX_OBJECTS]`.

**A finding, not a Task 3 defect, surfaced by measuring this task's own
delta: `g_objects` costs more than ADR 0040 estimated, and costs flash as
well as RAM.** ADR 0040 estimated "~40 bytes per object" (~2.5KB total).
`xtensa-esp32s3-elf-nm` on the built object shows `g_objects` is exactly
`0x3800` = 14,336 bytes — **224 bytes per object, 5.6x the estimate** — and
it lives in `.data`, not `.bss`: `RenderState::fg` defaults to `KF_WHITE`
(`0xFFFF`, non-zero), so the 64-object array is not all-zero and cannot be
zero-initialized at boot the way an all-zero array would be. A `.bss` array
costs RAM only, initialized once by the runtime's zero-fill; a `.data`
array of the same size costs that same RAM *and* an equal number of bytes
of flash, permanently, to hold the initial values copied in at boot. This
is `hakoniwaos/src/scene.cpp`'s file, not `sdk/lua/`'s, and is out of this
task's scope to fix — noted here because this task is what made the cost
visible, not because it introduced it.

## Not verified

No scene declared through this binding has rendered on real hardware. The
ESP-IDF cross-compile above is clean and the firmware size was measured
against a real `idf.py build` output, but "compiles, links, and reports a
plausible size" is not the same claim as "has been flashed and watched" —
the same distinction ADR 0040 draws for Core alone. `kf.on_button`'s
`kf_app_buttons_pressed()` read is code-reviewed against the exact function
`kf_screen_nav_frame()` already calls, but has no dedicated headless check
of its own: driving a real button press through this codebase's fixed,
frame-indexed input script (`headless_input.cpp`) would mean adding a
button-press window to the sequence every golden-checksum check shares,
which risks moving `headless_determinism`/`headless_fullscreen` for a
check this task does not need — the same tradeoff
`run_creature_screen_input_check()` already made by driving
`kf_creature_screen_debug_press()` instead of real input. Nothing
interactive declares a scene this task — the demo creature script still
only logs, `kf_lua_scene_declared_anything()` never returns `true` in
either the SDL build or the device build, and `kf_scene_commit()` is never
called from either frame loop tonight. Task 4 is the first task where a
scene reaches the panel from anywhere but this task's own headless check.
