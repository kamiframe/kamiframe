# ADR 0057: Giving the creature its voice — the sequence queue fix, duty cycle, `kf.melody()`, level-crossing want pings, and a persisted volume setting

**Status:** Accepted
**Date:** 2026-08-13

## Context

ADR 0055 built the sound foundation — `kf/hal/audio.h`, three backends, a
single square-wave tone, and one attention chirp. It named its own gap
plainly in `ports/esp32/hal/esp_audio.cpp`: a **depth-1 queue of individual
notes**, `xQueueOverwrite()`. That is correct for one tone, but every sound
in `tools/kf_chiptune.py`'s `SOUNDS` table — the approved set, 21 sounds
picked by ear by the owner from 20 candidates — is multi-note. Two
`kf_audio_tone()` calls in quick succession *replaced* each other on real
hardware, so a multi-note phrase would have played only its last note. This
task fixes that, and does five more things in the same pass, all requested
before it landed: duty-cycle control, a `kf.melody()` SDK surface, the
sounds wired to real game events, a level-crossing escalation system for
unmet wants (replacing an earlier timer-based design mid-task, per the
owner's own correction), and a persisted, cross-pet volume setting.

## Decision

### 1. The queue: whole sequences, depth 1 — not individual notes, depth N

Two shapes were available: deepen the queue so several *note* messages can
queue up and drain in order, or make the *sequence itself* (`kf_audio_note[]`,
count, duty) the unit that gets queued, keeping depth 1.

**One sentence of reasoning:** a per-note queue has a failure mode a
per-sequence queue does not — the want-ping system below re-pings on every
threshold a falling need crosses, and an ignored pet whose notes queued up
individually would eventually blurt out a backlog of stale chirps in a
burst; queuing whole sequences and replacing (not appending) on every new
`kf_audio_play_notes()` call keeps exactly one thing true at any moment —
the sound now playing is always the most recent thing the creature had to
say — while still playing each *sequence's own* notes in order, which is
the actual bug this fixes.

The other constraint this drove: the channel-enable/disable dance ADR
0055's own fix documented (enable, stream, silence-flush, disable) now
brackets a whole **sequence**, not each note within it — enabling and
disabling seven times for the longest sound (hatch/evolve) would be seven
times the audible-spit risk that fix was written to eliminate, and seven
separate windows where a mid-phrase `kf_audio_stop()` could race the
disable the way the original bug did. See `ports/esp32/hal/esp_audio.cpp`'s
own header comment for the full account, including why this file's
existing enable/disable invariant had to be read and preserved, not merely
worked around.

### 2. Duty cycle in the HAL

`kf_audio_tone_duty(hz, ms, duty_permille)` and `kf_audio_play_notes(notes,
count, duty_permille)` extend `kf/hal/audio.h` additively —
`kf_audio_tone(hz, ms)` keeps its exact existing signature and contract
(`duty_permille = KF_AUDIO_DUTY_FAT`, unchanged behaviour), and the SDL/
headless/ESP32 backends all synthesise the waveform through one shared
internal path so there is exactly one duty-aware oscillator per backend,
not two to keep in sync. `KF_AUDIO_DUTY_THIN` (an eighth, 125/1000) is the
creature's own voice, `KF_AUDIO_DUTY_MID` (a quarter) is ordinary
care-response jingles, `KF_AUDIO_DUTY_FAT` (a half) is system fanfares —
same oscillator, no extra cost, matching `tools/kf_chiptune.py`'s own named
tiers exactly.

`kf_audio_note::hz == 0` is a **rest** inside a sequence — a legitimate,
expected token (`want_rest`'s own `"E6:70 -:30 B6:120"`) — deliberately
distinct from `kf_audio_tone()`'s own `hz == 0`, which stays a caller
mistake (`KF_ERR_INVALID`). The two single-note entry points
(`kf_audio_tone`/`kf_audio_tone_duty`) still reject a zero `hz` explicitly,
before ever reaching the shared sequence path.

### 3. `kf.melody(spec, duty_permille)` in Lua

Takes the *same* note-name grammar `kf_chiptune.py` already parses —
`"E6:55 -:35 B6:85"` — not raw Hz pairs. `duty_permille` is optional
(default `KF_AUDIO_DUTY_FAT`, matching `kf.tone()`'s own implicit 50%);
`kf.DUTY_THIN`/`kf.DUTY_MID`/`kf.DUTY_FAT` are exposed as `kf` table
constants for a script to pass. The parser (`sdk/lua/kf_lua_port.cpp`)
uses the identical A4=440, MIDI-numbering formula `kf_chiptune.py`'s
`note_hz()` uses, via `std::pow` — floating point is fine in this file
specifically (`sdk/lua/` is not `hakoniwaos/`; `check_no_float.py` only
scans `hakoniwaos/src`/`hakoniwaos/include`) and runs once per note per
call, not per sample, so there is no per-frame cost. A malformed spec
(unrecognised note, too many notes) raises via `luaL_error()` — a script
author's typo, not data, the same line `kf.button()`'s unknown-name case
already draws.

### 4. The sounds, wired

`examples/creature_demo/creature.lua` now carries its own `SOUNDS` table,
mirroring `kf_chiptune.py`'s exactly (note specs, duty tier) — **both files
say, in their own header comments, that they must be changed together**:
`kf_chiptune.py` is the preview tool, `creature.lua` is the shipping copy.

| Event | Sound | Detected via |
|---|---|---|
| A want's own need crosses its first threshold | `want_food`/`want_play`/`want_rest`/`want_bath`/`want_flush` | level-crossing (see §5) |
| Escalation, ping 2 / ping 3 | `want_again` / `want_whine` | level-crossing |
| feed/play/rest/bath accepted, liked or neutral | `care_feed`/`care_play`/`care_rest`/`care_bath` | the SAME debounced button press `kf_home_screen_input.cpp` (called earlier the same frame) already used to apply the action |
| feed/play/rest/bath accepted, disliked | `care_disliked` instead | `pet.last_reaction() == 2`, read the same frame |
| flush | `care_flush` unconditionally | flush has no reaction to key off (kf/pet.h's own design) |
| Falls asleep / wakes | `sleep` / `wake` | `pet.asleep()` edge, both directions |
| Egg hatches | `hatch` | `announce_stage()`'s own `previous == "egg"` branch |
| Any later stage change | `evolve` | `announce_stage()`, every other transition |
| Dies | `death` | `pet.dead()` edge, screen-agnostic `on_frame()` |

`idle_warble`, `confused`, `menu_blip` are still deliberately unwired — no
call site was named for them.

Care-button detection reuses `kf.button("a"/"up"/"down"/"left"/"right")`,
the exact bits `kf_home_screen_input.cpp` already reads earlier the same
frame (`kf_lua_home_screen_frame()`'s own ordering: buttons handled, THEN
`on_home_frame()` runs) — so `pet.last_reaction()` is already the JUST-
applied result by the time creature.lua reads it, not stale. One subtlety
this drove: the guard has to use `raw_asleep` (captured *before* a manual
`pet.wake()` call this same frame can flip the local `asleep` variable),
not the post-wake value — otherwise pressing A to wake a sleeping creature
would also play `care_feed` the same frame, even though
`kf_home_screen_input.cpp`'s own asleep guard means no feed actually
happened.

### 5. Want pings: level-crossing, not a timer — a mid-task correction

The original brief asked for a conservative re-chirp *interval* with an
escalation count. The owner corrected this before it was built, with a
better design: **"ping once when the care level gets to that level, then
ping again about half the rest of the way to zero, then one urgent ping at
5 points/units before 0."** Three pings per unmet need, at level crossings,
no timer — self-limiting by construction, since a need that stops falling
stops crossing thresholds on its own.

| Need | Ping 1 | Ping 2 | Ping 3 (urgent) |
|---|---|---|---|
| FOOD / PLAY / REST (drain toward 0) | 25% (`KF_PET_WANT_*_ON_MP`) | 12.5% | 5% |
| BATH (dirtiness, rises) | 80% (`KF_PET_WANT_BATH_ON_MP`) | 90% | 95% |
| FLUSH (poop count, rises, max 8) | 3 (`KF_PET_WANT_FLUSH_ON_POOPS`) | 5 | 7 |

BATH and FLUSH don't fall toward zero, so the owner confirmed mirroring
the *intent* (three evenly-spaced pings, worst last) rather than the
literal "halfway to zero" formula. FLUSH's middle ping is 5, not 6 — the
owner's own final call, so the three gaps (3→5→7) are equal.

Implementation: one latched integer level (0..3) **per need**, computed
fresh every frame from the raw need value (`pet.hunger()` etc., not
`pet.wants()`'s priority-resolved single want — a need pings even while a
*different* want has display priority). A ping sound fires only when the
level goes **up**; the level dropping — whether from care or simply not
having fallen further — is always silent, which is exactly "the latch
resets when the need recovers back above the level." Ping 1 plays the
want's own call (`want_food` etc.); ping 2 is always `want_again`; ping 3
is always `want_whine` — the same two escalation rungs across every need,
matching `kf_chiptune.py`'s own "two rungs... the SAME creature, not a new
alarm." The level is tracked every frame regardless of sleep state; only
the *sound* is gated by `not asleep`, so a night's worth of unprotected
decay (ADR 0053) doesn't surface as a burst of pings the instant the
creature wakes — it silently reflects wherever the level already caught up
to.

### 6. A persisted, cross-pet volume setting

Five positions — OFF, 1, 2, 3, 4 — added to the Settings screen as a new
field (HOUR → MINUTE → AM/PM → **VOLUME** → SAVE), edited locally like
every other field and committed only on SAVE, applied live to the audio
HAL (`kf_audio_set_volume()`) at that same commit.

**The persistence requirement had a trap, and it shaped the whole design.**
"Persist globally through multiple pets" means the value must survive
`kf_pet_session_debug_reset()` and a fresh egg — so it cannot live in
`kf_pet_state`, which that reset wipes entirely. `kf/settings.h` is a new,
deliberately tiny store: one versioned byte blob under its own key
(`KF_SETTINGS_SAVE_KEY = "settings"`, separate from `kf/pet.h`'s
`KF_PET_SAVE_KEY = "pet"`), sized to hold whatever the *next* global,
cross-pet preference turns out to be too — a one-key-per-option store
would be the wrong shape for a device about to have a second one of these.
Mirrors `kf/pet.h`'s own pack()/unpack()/save/load shape exactly, scaled
down. Loaded once at boot (`hakoniwaos/src/app.cpp`, right after
`kf_audio_init()`) and applied to the live audio HAL immediately, so every
sound the process ever plays — starting with the first — obeys whatever
was last saved.

`KF_VOLUME_OFF` means **genuinely silent**: every backend skips producing
or queuing output entirely rather than playing a zero-amplitude sequence —
provably silent, not merely quiet. Levels 1–4 scale amplitude linearly
(1/4, 2/4, 3/4, 4/4 of full) in each backend's own waveform synthesis, not
in `creature.lua`, so every sound — including any added later — obeys it
automatically.

## What Chris needs to know

- **The old single 880Hz/150ms attention chirp is gone**, replaced entirely
  by the level-crossing want-ping system. `kf.tone()`/`kf.beep()` still
  exist and still work (Part B's own tests, unchanged) — nothing in this
  task removed them from the SDK — they are simply no longer what fires
  the want signal.
- **Every duty tier, note spec, and ping threshold is feel**, flagged for
  the board: `KF_AUDIO_DUTY_THIN/MID/FAT`'s exact permille values, the
  three ping bands per need, `KF_SETTINGS_DEFAULT_VOLUME` (chosen as
  `KF_VOLUME_4`, closest to the pre-existing always-full behaviour), and
  the 1/4-per-level volume curve (linear, not perceptual/logarithmic).
- **`SOUNDS` in `creature.lua` and `tools/kf_chiptune.py` must be changed
  together** — both files now say so in their own header comments.
- Nothing in `SOUNDS`' actual note specs looked wrong on inspection; they
  were not touched, per this task's own instruction.

## The proof

`ctest --test-dir build` — **53/53** (same count as ADR 0055 reported;
this task substantially expanded `audio_check` and `settings_screen_check`
in place rather than registering new top-level ctest entries, since both
already covered exactly this feature area's lettered sections).

`python3 tools/check_no_heap.py .` — `core is heap-free (39 files
scanned)` — up by one file (`hakoniwaos/src/settings.cpp`; `poison.h` is
excluded by name, hence 39 of 40).

`python3 tools/check_no_float.py .` — `core is float-free (40 files
scanned)`.

Every `kamiframe-headless` run stays **silent** — unchanged from ADR
0055: `headless_audio.cpp` never opens a device, never links SDL.

Audio touches **no dirty rect anywhere** — confirmed by grep across
`kf/hal/audio.h`, all three backends, and `sdk/lua/kf_lua_port.cpp`: the
only hit is a comment analogy, not a call.

Non-blocking on every backend, confirmed by reading each one: headless is
pure state update; SDL synthesises the whole (bounded, ≤`KF_AUDIO_MAX_
SEQUENCE_MS`) buffer up front and hands it to `SDL_PutAudioStreamData()`
once, same shape ADR 0055 already established for a single tone, now
extended to a sequence; ESP32 posts one message to a queue and returns,
the dedicated task does the actual blocking I/O.

### Non-vacuity: every new assertion broken and watched fail, then restored

| Assertion | How it was broken | Exact failure message(s) |
|---|---|---|
| A want-ping fires exactly once per level crossing, not once per frame the level holds | (Described in the check's own comment, verified by temporarily removing `update_ping()`'s `current_level > previous` edge check) | `FAILED: hunger holds at ping1 for 300 more frames: still exactly one ping...` with a melody count of 301 instead of 1 |
| A disliked care action plays `care_disliked`, not the accepted sound | Computed via `kf_pet_reaction_to()` against a real disliked trait, not assumed | `FAILED: a disliked feed plays care_disliked INSTEAD...` (caught and fixed during this task's own testing: the per-action variation counter meant the second feed press used a different variation than the reaction was computed against — `kf_home_screen_input_reset_variations_for_test()` fixed it) |
| Falling asleep plays `sleep`; staying asleep plays nothing further; waking plays `wake` | Verified as three separate counted assertions across a held sleep window | Each checked against `kf_headless_audio_melody_count()` deltas |
| Asleep with hunger critical: no ping | Verified end-to-end via the real demo script, accounting for the sleep-edge sound itself firing on the same first frame (a real test-design bug caught during this task: the naive version failed because it didn't isolate the sleep edge from the want-ping suppression it was actually trying to prove) | `FAILED: asleep with hunger critical: no ping...` before the fix separated the two |
| `KF_VOLUME_OFF` is genuinely silent | Checked that neither `kf_headless_audio_tone_count()` nor `_melody_count()` moves, only `kf_headless_audio_muted_count()` does | Distinguishes "silenced" from "never recorded a request at all" |
| The volume setting survives a pet reset | `kf_pet_session_debug_reset()` called, then the LIVE volume checked (proves the reset doesn't touch it), then the in-memory HAL value deliberately perturbed and a FRESH `kf_settings_load()` checked (proves actual storage persistence, not just an untouched variable) | Two-part check specifically designed so passing it could not be an accident |

## What is unverified

**Nothing in `esp_audio.cpp` has been run on a speaker.** `idf.py
-DKF_PANEL=ili9341 build` is clean — zero warnings or errors, including
every file this task touched. Firmware image: **578,320 bytes** (63% of
the 1,572,864-byte app partition free). Not flashed, per this task's own
instruction. I2S still has no acknowledgement the way I2C does — a board
with nothing soldered to BCLK/WS/DOUT will run this driver's entire
sequence-queue logic successfully and produce no sound at all, silently,
exactly as ADR 0055 already documented for the single-tone version.

## What changed in the plan

No plan document exists for this task — dispatched directly, then amended
twice more by the owner mid-task (the level-crossing correction in §5, and
the FLUSH ping-2 value confirmed as 5 rather than left as "5 or 6"). Both
corrections are folded into this ADR as the accepted design, not appended
as an afterthought — there is no earlier "timer-based" version of this
feature anywhere in the shipped code or tests for a future reader to trip
over.
