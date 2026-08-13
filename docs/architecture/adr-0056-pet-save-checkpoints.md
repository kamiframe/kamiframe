# ADR 0056: Real save checkpoints — after care, at sleep, and a dirty-gated periodic net

**Status:** Accepted
**Date:** 2026-08-12

## Context

CLAUDE.md names the platform's central promise: "RTC with backup (the pet
must age while powered off)." Nothing in this codebase actually delivered
it on device. Chris verified the defect himself before asking for this
work, and the verification holds:

- `kf_pet_session_save()` was previously called from exactly one place,
  `kf_pet_session_shutdown()`.
- On ESP32, `kf_pet_session_shutdown()` sits after `app_main()`'s main
  loop, and that loop's own comment (`ports/esp32/main/app_main.cpp`) calls
  it **"unreachable in practice"**: `esp_input.cpp`'s `kf_input_poll()`
  hardcodes `quit_requested = false` (there is no hardware "quit" concept),
  so `kf_app_frame()` never returns false and the `for(;;)` never exits.
- Care actions (`kf_pet_session_feed()` and its four siblings) mutated
  state and pushed a debug snapshot, but never saved.
- Nothing saved on a timer.
- `pet.save()` existed as a Lua binding, but `examples/creature_demo/
  creature.lua` never called it.

So every real boot found no save on NVS and hatched a fresh egg — a
battery pet that forgets it existed the instant it loses power, which on
a device unplugged or run to a dead battery is *every* power-off. The
desktop simulator hid this: closing the SDL window reaches
`kf_pet_session_shutdown()` through a normal `main()` return, so the one
save path that worked happened to be the one path a real device never
takes. Exactly the desktop/device divergence the shared-HAL design exists
to catch, hiding in the one place it doesn't reach: *when the host decides
to save*.

Chris, asking for this task: "add in the real save state for my pet to
flash to the device as well. I want it to save in some way on low power
mode."

Two things this task deliberately does **not** do, named up front:

- **Device deep sleep.** `kf_power_deep_sleep_until()` (`kf/hal/power.h`)
  has a real ESP32 backend (`ports/esp32/hal/esp_power.cpp`) but, verified
  by grep before writing a line of this task, **no production caller** —
  every call site is `simulator/src/headless/headless_main.cpp` test code
  or the desktop/headless `kf_power_deep_sleep_until()` backend
  implementations themselves. So "low power mode" here can only mean the
  pet's own `asleep` state (ADR 0048), not a CPU power state. If a future
  deep-sleep path needs a save hook, it belongs right before whatever calls
  `kf_power_deep_sleep_until()` — that call site does not exist yet, so
  this task does not add one.
- **A migration path.** Nothing about the save format changed. `KF_PET_
  SAVE_BYTES` is still 109 (unchanged since ADR 0053).

## Decision

### 1. Three automatic checkpoints, all funnelling through `kf_pet_session_save()`

`kf_pet_session_save()` is still the one function that calls `kf_pet_save()`
— but it is no longer called from exactly one place. `simulator/src/pet/
kf_pet_session.cpp` now calls it automatically at three points, all inside
that same file so both desktop and ESP32 get it for free (`kf_pet_session.
cpp` compiles into the ESP32 firmware unchanged — see `ports/esp32/main/
CMakeLists.txt`):

1. **After every care action** — `feed`/`play`/`rest`/`bath`/`flush`/`wake`/
   `tuck_in`. These are rare, human-paced events (a button press, edge-
   detected — see "What this is not exposed to" below), never a per-frame
   path, so an immediate save costs nothing worth gating. Guarded against
   the one real waste this would otherwise cause: every one of `kf/pet.h`'s
   care functions is a **documented no-op** under some condition of its own
   (`state->dead` for feed/play/rest/bath/flush; `state->dead ||
   !state->asleep` for wake; `!kf_pet_drowsy(state)` for tuck_in — each
   function's own header comment). `kf_pet_session.cpp` reads the *same*
   condition Core's own early return checks before deciding whether to
   save, so a shrine screen a player can still poke at, or a script that
   calls one of these against an inapplicable pet, does not cost the
   device a flash write for a press that changed nothing.

2. **At the awake → asleep transition**, detected live inside
   `kf_pet_session_frame()`'s flush (comparing `state->asleep` before and
   after `kf_pet_advance()`). This is Chris's "low power mode": the
   creature is about to do nothing for roughly nine hours, so it is the
   natural checkpoint. `kf_pet_state::asleep` already existed (ADR 0048)
   and the transition was already detected inside Core
   (`apply_stage_segment()`, `hakoniwaos/src/pet.cpp`) — this task only
   adds the session layer noticing the same edge and saving on it.

3. **Periodically while running**, gated on both "due" (a real, monotonic
   interval — see below) and "dirty" (a private flag: has anything
   happened since the last save that a checkpoint of any kind hasn't
   already covered). The idle case this actually protects, verified rather
   than assumed: a **dead** pet. `kf_pet_advance()`'s own leading `if
   (state->dead) return;` (`hakoniwaos/src/pet.cpp`) means a flush against
   an already-dead pet changes *nothing* — no decay, no accumulator, not
   even `last_advanced`. The dirty flag is set `true` only when a flush
   started from a *not-yet-dead* state, so once a pet dies and that death
   is captured by one save, every later periodic tick against the same
   dead pet attempts nothing further. (An egg, by contrast, *is* marked
   dirty every flush — its needs don't decay, but `stage_elapsed_seconds`
   still advances — which is harmless: its periodic checkpoint is still
   bounded by the same interval, not every flush.)

Every checkpoint is an ordinary call to `kf_pet_session_save()` — there is
no separate "automatic save" code path to drift from the manual one (the
SDL debug window's Save button, and Lua's `pet.save()`). A failed write
(`KF_ERR_EXHAUSTED`, a full partition) leaves the dirty flag set and does
**not** push the periodic deadline forward, so the very next checkpoint of
*any* kind retries it — not only the next scheduled interval.

**What this is not exposed to:** care-action saves ride real button
presses, which `hakoniwaos/src/app.cpp` edge-detects (`buttons_pressed_
edge = buttons_stable & ~previous`), not held-button repeats — a player
holding Feed down does not spam a save every frame. Verified by reading
`kf_app_buttons_pressed()`'s implementation before relying on it, not
assumed.

### 2. The periodic interval and the endurance arithmetic

`KF_PET_SESSION_PERIODIC_SAVE_SECONDS = 600` (10 minutes), real monotonic
time (`kf_time_mono_us()`), deliberately **not** scaled by `KFDBG MULT`'s
time multiplier — NVS wear is a property of real elapsed time on the
device, not simulated pet-time.

**The arithmetic, grounded in this project's own vendored ESP-IDF v6.0.2
(`~/esp/esp-idf`, the exact version `BUILDING.md` names), not memory or a
guess:**

- `KF_PET_SAVE_BYTES` is 109 (unchanged, `kf/pet.h`).
- NVS entries are fixed at 32 bytes each (`NVS_CONST_ENTRY_SIZE`,
  `components/nvs_flash/private_include/nvs_constants.h`).
- A page holds `NVS_CONST_ENTRY_COUNT = 126` entries (that header's own
  constant — a 4096-byte page, less a 32-byte page header and a 32-byte
  entry-state bitmap, divided by 32).
- Every `nvs_set_blob()` write, **verified by reading `Storage::writeItem()`
  in `components/nvs_flash/src/nvs_storage.cpp`**, goes through the
  blob-index scheme regardless of size: one `BLOB_IDX` entry (a small
  fixed-size record — namespace, chunk count, chunk-start offset — 1 entry)
  plus one `BLOB_DATA` item of `1 + ceil(dataSize / 32)` entries. For 109
  bytes: `1 + ceil(109/32) = 1 + 4 = 5` data entries, **+ 1 index entry =
  6 entries per save**, 192 bytes physically written for a 109-byte
  logical value.
- This project's `nvs` partition (`ports/esp32/partitions.csv`) is `0x6000`
  = 24576 bytes = **6 pages** of 4096 bytes each, and holds **only** the
  `"pet"` key — verified by grepping every `kf_store_write()` call site in
  the repo; `kf_pet_save()` is the only caller.
- 126 entries ÷ 6 entries/save = **21 saves fill one page** before it needs
  compaction (an erase). Because this partition holds only one key, a
  compaction reclaims essentially the whole page (at most one live entry
  group survives), so the freed page re-enters the same 21-saves-per-fill
  cycle. NVS also **skips the write entirely** if the new blob is
  byte-identical to the stored one (`Storage::writeItem()`'s own
  `cmpMultiPageBlob()` check) — a second, independent safety margin
  underneath the dirty flag above, verified in the same read, not assumed.
- Individual physical pages absorb erases roughly evenly across the
  6-page pool (NVS's own page rotation), so **erases per individual
  page ≈ saves ÷ (21 × 6) = saves ÷ 126**.
- Rated endurance: **100,000 erase cycles**, the standard conservative
  planning figure for the class of SPI NOR flash these modules use. This
  leg is *not* pulled from the WROOM-1 module's own datasheet (not on
  hand) — flagged plainly as the widely-used conservative baseline, not a
  part-specific verified number, unlike the NVS structure above.

**Sustainable saves before the most-worn page hits that rating:**

- Realistic model (crediting NVS's own page-fill batching): `126 ×
  100,000 = 12,600,000` saves.
- Maximally pessimistic floor (crediting *no* batching at all — one write
  treated as one erase, spread only across the 6 physical pages):
  `6 × 100,000 = 600,000` saves.

**Writes per day, worst case.** Assume the periodic checkpoint fires on
every single interval for the pet's entire runtime (i.e. the dirty flag
never once gets to say no — strictly worse than reality, since a dead or
otherwise-still pet costs nothing): `24 × 60 ÷ 10 = 144` periodic saves/
day. Add a generous 50 care-action saves/day (a far more active player
than this loop realistically produces — button presses are edge-triggered
and human-paced) and 1 sleep-transition save/day. Round up to **200
writes/day** for margin.

- Writes/year: `200 × 365 = 73,000`.
- **Realistic model:** `12,600,000 ÷ 73,000 ≈ 173 years` before the
  most-worn page reaches its rated cycles.
- **Pessimistic floor:** `600,000 ÷ 73,000 ≈ 8.2 years`, even crediting
  none of NVS's own batching and assuming the device is never idle for a
  single one of its periodic windows across its whole life.

Both numbers comfortably clear a plausible product lifetime, and the
pessimistic one is deliberately punishing — real usage is not "dirty every
ten minutes forever," it is bursts of play against long idle and sleeping
stretches the dirty flag turns into zero-cost checkpoints. **The dirty
flag is still the right first move Chris asked for**: without it, "save
every 30 seconds forever" (a 20x tighter interval than what shipped, with
no idle exemption at all) would burn through even the realistic model in
roughly 9 years and the pessimistic floor in under 5 months — a genuine
hardware-lifetime bug, not a nitpick.

### 3. The load path was checked critically, not assumed correct

`kf_pet_load_and_advance()` (`hakoniwaos/src/pet.cpp`) already existed and,
read closely, already does the right thing: it re-baselines `last_advanced`
to the current wall clock on every call (whether or not a save was found),
clamps a backwards-jumped clock to zero elapsed rather than ageing the pet
negatively, and defers ageing entirely (leaving `last_advanced` invalid)
when the wall clock itself isn't set yet. None of that needed to change.
What this task adds is a save on the *write* side that this read path had
never, on a real device, actually been given anything to load. See
`run_pet_check()`'s existing equivalence checks (`--verify-pet`) for the
load path's own correctness proof, unchanged by this task, and this ADR's
new `run_pet_save_checkpoints_check()` (`--verify-pet-save-checkpoints`)
for proof the two now actually meet on device-shaped conditions — a save
written by a care-action checkpoint with **no** explicit save call and
**no** `kf_pet_session_shutdown()` in between, reloaded independently, aged
correctly for the elapsed wall-clock time.

### 4. A save's measured cost, and the frame-budget question

`run_pet_save_checkpoints_check()`'s sixth case times 500 consecutive
`kf_pet_session_save()` calls with `std::chrono::steady_clock` and logs the
result. Measured on this desktop build: **~110-140 µs/save** — this is the
**desktop `host_storage.cpp` backend's** cost (`fopen` + `fwrite` +
`fflush` + `fsync` + `rename`, through the host OS filesystem), reported
because it is what could actually be measured here, and it is **not** a
stand-in for ESP32 NVS's cost. Said plainly rather than glossed over: those
are two structurally different write paths (a filesystem sync vs. a direct
SPI-flash blob commit), and this task did not flash real hardware to
measure the real number (Chris flashes, per this task's own instructions).

What the NVS-side arithmetic above implies for frame budget, stated as
what it is — an estimate, not a measurement: an ordinary `nvs_set_blob()` +
`nvs_commit()` with no page compaction needed is typically a low-single-
digit-millisecond SPI flash program operation, comfortably inside the
33.3ms (30fps) frame budget. The case worth naming plainly: **once every
~21 saves**, the write lands on a full page and triggers a compaction —
copying surviving entries forward and erasing a 4KB sector, which on
typical SPI NOR flash costs tens of milliseconds, potentially exceeding a
single frame's budget on that specific frame. At the 10-minute periodic
interval this is roughly once every 3.5 hours in the worst (always-dirty)
case — a rare, bounded hitch, not a sustained problem, but a real one and
not hidden here. If a future bench measurement on real hardware shows this
hitch is actually visible or audible, the fix is straightforward and
already scoped: move the save call off the synchronous per-frame path (a
"save requested" flag consumed once outside `kf_pet_session_frame()`'s own
timing, or a dedicated task) rather than changing when checkpoints happen.
Not built here because it is not yet known to be needed — the desktop
number is fast, the ESP32 number is estimated, and Chris asked this task
not to build ahead of what bring-up has actually shown.

## Consequences

- The platform's stated promise — the pet ages while powered off — now has
  something real behind it on the actual failure mode a battery device
  sees: an abrupt loss of power, not an orderly shutdown.
- `KF_PET_SESSION_PERIODIC_SAVE_SECONDS` and the arithmetic above are
  coupled — changing one without revisiting the other is exactly the kind
  of drift CLAUDE.md's "keep the plan documents true" section warns about;
  `kf_pet_session.h`'s own comment on the macro points back here.
- The outstanding bench item this unblocks: an offline fast-forward test
  against a *real* saved pet on device, over a real power cycle — this was
  meaningless before this task (there was never a save to fast-forward
  from) and is now possible, but has not been run on hardware as part of
  this task.
- Device deep sleep remains unbuilt, on purpose — see "Context" above.

## What was verified before writing this, and what was not

- **Verified:** `kf_pet_session_shutdown()` was the sole caller of
  `kf_pet_session_save()` before this task (grep, then read every call
  site). `kf_power_deep_sleep_until()` has no production caller (grep,
  then read every call site — all are `headless_main.cpp` test code or the
  three HAL backend definitions themselves). `kf_pet_advance()` is a true
  no-op for an already-dead pet (read `hakoniwaos/src/pet.cpp`'s leading
  `if (state->dead) return;`, both in `kf_pet_advance()` and in every care
  function it guards). Button presses reach `kf_home_screen_handle_care_
  buttons()` edge-detected, not held-repeat (read `hakoniwaos/src/app.cpp`'s
  `buttons_pressed_edge` computation). Every NVS structural number in the
  endurance arithmetic above (entry size, entries per page, entries per
  blob write, the built-in identical-value skip) — read directly from this
  project's own vendored ESP-IDF v6.0.2 source, not recalled from memory.
  `ctest --test-dir build` was 52/52 at the branch point before any change
  in this task, confirmed by running it.
- **Not verified (explicitly out of scope, named rather than hidden):** the
  actual ESP32 NVS write/commit timing, and the actual page-compaction
  stall duration, on real silicon. The 100,000-erase-cycle rating is the
  standard planning figure for this class of part, not pulled from the
  WROOM-1 module's own flash datasheet. The offline fast-forward against a
  real saved pet, across a real power cycle, on the bench.
