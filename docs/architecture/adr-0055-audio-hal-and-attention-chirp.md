# ADR 0055: The sound foundation — `kf/hal/audio.h`, three backends, `kf.tone()`/`kf.beep()`, and a voice for the attention signal

**Status:** Accepted
**Date:** 2026-08-12

## Context

ADR 0050 (the attention signal) shipped a pet that pulses a silent `!` on
screen when it wants something, and named its own gap plainly: "there is no
`kf/hal/audio.h` and no haptic HAL in this repo," and "when sound or
haptics arrive, they hang off the identical `KF_PET_WANT_NONE -> something`
transition `creature.lua`'s `want`/`was_wanting` local already computes
every frame -- one call site, already isolated." This task builds that HAL,
gives it three backends, exposes it to Lua, and uses it at exactly that
call site.

Two things make this task different from every other HAL slice so far:

- **The device has no speaker wired.** `ports/esp32/hal/kf_esp_pins.h`
  reserves I2S pins (BCLK GPIO1, WS GPIO2, DOUT GPIO9, DIN GPIO47) and says
  outright they are "reserved, never wired," and that the passive buzzer
  this project owns "is not given a pin at all" because it is "redundant
  with the amplifier." So the ESP32 backend this task writes is real code
  against a real driver, compiled and never run.
- **Chris owns the parts.** Two MAX98357A I2S class-D amps, four 1W 8Ω
  speakers, five frequency-controllable passive buzzer modules, five INMP441
  microphones — ordered, on hand, not yet soldered. This is bring-up
  discipline exactly as ADR 0020 described it for `esp_power.cpp`: implement
  the contract correctly, document exactly what is unverified, stop there.

## Decision

### 1. One capability: a tone, not sampled playback

The target hardware has two genuinely different ways to make sound — the
I2S amp (real sampled audio, any waveform) and a passive buzzer (a GPIO
toggled at a frequency, nothing more). `kf/hal/audio.h` exposes only the
buzzer's capability: `kf_audio_tone(hz, ms)`, a square wave at an integer
frequency for an integer duration in milliseconds. No
`kf_audio_play_pcm(...)`.

**One sentence of reasoning:** a tone is the floor every backend can
actually satisfy today — a buzzer directly, an I2S amp by synthesising the
waveform in software, a desktop speaker via SDL3, and "nothing wired" by
doing nothing — while a sampled-playback call would exist to satisfy the
one backend that has an amp, with every other backend implementing it as
"cannot," and there is no audio asset pipeline in this repo to author a
clip with in the first place. Building that surface now would be building
ahead of what anything can use, which is exactly what CLAUDE.md's hardware
section warns against. `kf_esp_pins.h`'s own comment already made this call
implicitly — the buzzer has no pin because it is "redundant with the
amplifier," meaning the amp is expected to produce a tone too, in software.
When real sampled audio earns its own asset format and packer, it earns its
own HAL call then.

The contract, in full (`hakoniwaos/include/kf/hal/audio.h`):

```c
kf_result kf_audio_init(void);
kf_result kf_audio_tone(uint32_t hz, uint32_t ms);
void      kf_audio_stop(void);
void      kf_audio_shutdown(void);
```

- `KF_AUDIO_MIN_HZ` (20) / `KF_AUDIO_MAX_HZ` (20000) — a sanity floor
  against an obvious mistake, not a specific buzzer's datasheet (none of
  Chris's five modules are on the bench to measure).
- `KF_AUDIO_MAX_MS` (5000) — a ceiling against a script bug turning "chirp"
  into "drone," on a device with a speaker centimetres from someone's ear.
- `hz == 0` is `KF_ERR_INVALID`, deliberately not "silence" — the correct
  way to ask for silence is not calling this function, or calling
  `kf_audio_stop()`. A magic zero-frequency value would be exactly the kind
  of not-obvious special case `docs/sdk-style-guide.md` asks the layer
  above this one to avoid.
- **Non-blocking.** `kf_audio_tone()` starts the tone and returns
  immediately; every backend below plays it out in the background. A
  blocking call would stall the frame loop for the tone's whole duration,
  which a 30fps budget (`kf/budget.h`) cannot absorb for even a 150ms
  chirp.
- **Silence is the safe default.** A backend with nothing to make sound
  with returns `KF_ERR_UNAVAILABLE` and does nothing else — never a crash,
  never a hang, the same contract `esp_time.cpp` documents for a missing
  DS3231, applied here to a missing speaker.
- No heap, no floats in the header itself; every backend implementation
  either owns a fixed buffer or streams samples on the fly, and every
  waveform below is generated with integer period math, not because
  anything scans backend files for it (`tools/check_no_heap.py` only scans
  `hakoniwaos/src`/`hakoniwaos/include`) but because there is no reason to
  reach for a float when integer division gives the identical square wave
  a buzzer's own GPIO toggle would.

### 2. Three backends

**Headless (`simulator/src/headless/headless_audio.cpp`)** — records what
it was asked to do and makes no sound at all. This is what makes the whole
feature testable: `kf_headless_audio_tone_count()`,
`kf_headless_audio_last_hz()`/`_last_ms()`, `kf_headless_audio_stop_count()`,
and `kf_headless_audio_reset()` (needed because `run_audio_check()` runs
several self-contained sections back to back inside one process, unlike the
checksum/frame counters other headless probes accumulate for a whole
process). Built and designed first, per this task's own instruction.

**Desktop/SDL3 (`simulator/src/sdl/sdl_audio.cpp`)** — audible on Chris's
Mac. SDL3's audio-stream API (`SDL_OpenAudioDeviceStream`,
`SDL_PutAudioStreamData`) does the mixing and device I/O; this file
generates a mono 16-bit square wave (44.1kHz, integer period math, no
float) and hands the whole buffer to SDL in one call — nothing here plays
longer than `KF_AUDIO_MAX_MS` (5s), so generate-then-hand-off is far less
code than a streaming callback for no real benefit. `SDL_ClearAudioStream`
gives `kf_audio_tone()`'s "replaces, not queued behind it" contract and
`kf_audio_stop()`'s immediate silence.

**ESP32 (`ports/esp32/hal/esp_audio.cpp`)** — I2S in standard master/TX-only
mode on the already-reserved BCLK(GPIO1)/WS(GPIO2)/DOUT(GPIO9) pins,
targeting the MAX98357A amp with a software-synthesised square wave — the
same waveform a passive buzzer would produce directly, carried over a
different wire, which is what makes "one HAL capability, several ways to
satisfy it" concrete rather than a slogan. Non-blocking per the HAL's own
contract: `kf_audio_tone()` only validates and posts a message to a
depth-1 FreeRTOS queue (`xQueueOverwrite`, matching the "replaces, not
queued behind it" contract exactly), and a dedicated task created at init
does the actual blocking `i2s_channel_write()` calls in small chunks,
polling an atomic stop flag between chunks so `kf_audio_stop()` can
interrupt mid-tone. Every init failure path — channel creation, standard-
mode init, enable, queue creation, task creation — logs a warning and
leaves the backend un-ready rather than failing `kf_audio_init()` itself,
matching `esp_time.cpp`'s "never a crash, never a hang" contract for a
missing DS3231.

**`esp_driver_i2s` added to `ports/esp32/main/CMakeLists.txt`'s
`KF_MAIN_REQUIRES`** — the one new ESP-IDF component this task pulls in,
alongside the existing `esp_driver_i2c`/`esp_driver_gpio`/etc.

### 3. `kf.tone(hz, ms)` / `kf.beep()`

`sdk/lua/kf_lua_port.cpp` adds two flat functions, not one generic
dispatch, per `docs/sdk-style-guide.md`'s own rule:

- `kf.tone(hz, ms)` — the primitive. Returns `true`/`false`, never raises:
  an out-of-range `hz`/`ms` is treated as **data**, the same choice
  `lua_pet_reaction_to()`'s own out-of-range handling makes elsewhere in
  this file — a pitch is far more likely to come from arithmetic than a
  button name a script author typed by hand (`kf.button()`'s unknown-name
  case, which *does* raise, is the other side of that same style-guide
  line).
- `kf.beep()` — zero arguments, `kf.tone(880, 80)` spelled out as its own
  function, the same reasoning `kf.time()` exists rather than requiring
  every script to call `kf.hour()`/`kf.minute()` and format the string
  itself.

### 4. The chirp

`examples/creature_demo/creature.lua`, exactly the call site ADR 0050
named: inside the existing `if not was_wanting then want_elapsed_ms = 0
... end` block, the same false→true edge that already resets the `!`'s
blink cycle.

```lua
local kWantChirpHz = 880   -- A5; a plain, unremarkable pitch, not a chosen note
local kWantChirpMs = 150   -- long enough to notice across a room, short
                           -- enough to read as one chirp, not a tone
...
if not was_wanting then
    want_elapsed_ms = 0
    if not pet.asleep() then
        kf.tone(kWantChirpHz, kWantChirpMs)
    end
end
was_wanting = true
```

**Once per want streak, never per frame** — the `if not was_wanting`
guard is what makes this tasteful and rare rather than something that gets
muted or thrown across a room (this task's own brief, in those words).

**The `pet.asleep()` guard is honest, documented defense-in-depth, not
independently provable tonight.** `kf_pet_wants()`
(`hakoniwaos/src/pet.cpp`) already gates dead/asleep unconditionally,
*before* returning any want at all (ADR 0050) — so `want` above is never
non-nil while asleep, and `creature.lua`'s own `bed_shown` branch never
reaches this code at all while tucked in either. That means this specific
local guard is, today, unreachable in every real play session. It is kept
anyway, so a future reorder of either gate does not have to rediscover the
rule to avoid reintroducing a pet that beeps in its sleep — but this task
did **not** fabricate a break-and-restore test for that single line, because
doing so honestly would mean weakening `kf_pet_wants()`'s own dead/asleep
check, which lives in `hakoniwaos/src/pet.cpp` — other agents' explicit
territory tonight, not this task's to touch. What *is* independently
verified: `run_audio_check()`'s Section D drives the real demo script with
the pet asleep and hunger critical and asserts zero chirps, end to end,
using the real recording backend — a genuine regression guard, just one
whose only failure mode left to break is inside code this task was told to
stay out of. `run_attention_signal_check()`'s own Part A7 (unchanged by
this task) already breaks and restores the actual gate this all rests on.

## The proof

`ctest --test-dir build` — **52/52** (the prior 51 plus `audio_check`).

`python3 tools/check_no_heap.py .` — `core is heap-free (37 files
scanned)` — up from 36 by exactly one file, `kf/hal/audio.h` (the only new
file this task added under `hakoniwaos/`; `hakoniwaos/src/app.cpp`'s own
edit — one new `kf_audio_init()`/`kf_audio_shutdown()` call pair — added no
heap use and no floats, checked by hand, since there is no automated float
scanner).

Every `kamiframe-headless` run stays **silent** — `headless_audio.cpp`
never opens a device, never links SDL. Confirmed by running the full suite
with the sound turned off at the OS level and by reading the backend
itself: there is no code path to a speaker in it at all.

### Non-vacuity: every new assertion broken and watched fail, then restored

| Assertion | How it was broken | Exact failure message(s) |
|---|---|---|
| Out-of-range `hz` is rejected by the headless backend and records nothing | Short-circuited the range check in `headless_audio.cpp` (`if (false && (hz < KF_AUDIO_MIN_HZ ...))`) | `FAILED: hz below KF_AUDIO_MIN_HZ is rejected` / `FAILED: hz above KF_AUDIO_MAX_HZ is rejected` / `FAILED: none of the four rejected calls above changed what was recorded -- an invalid call makes no sound, not a different one` |
| `kf.tone(hz, ms)` reaches the HAL with `hz`/`ms` the right way round | Swapped the two arguments in `lua_kf_tone()`'s call to `kf_audio_tone()` | `FAILED: kf.tone(523, 200) reached the HAL with hz and ms the right way round, not swapped` (plus a downstream cascade failure in Part C, since it depends on the same call) |
| `kf.beep()` plays its own fixed default, not whatever `kf.tone()` last played | Changed `kBeepDefaultMs` from 80 to 999 | `FAILED: kf.beep() plays its own fixed default (880 Hz, 80 ms), distinct from whatever kf.tone() was last called with` |
| The chirp fires exactly once per want streak, not once per frame | Moved `creature.lua`'s `kf.tone(kWantChirpHz, kWantChirpMs)` call outside its `if not was_wanting then` guard | `FAILED: wanting FOOD: exactly one chirp fired...` / `FAILED: wanting FOOD continuously for 300 more frames: still exactly one chirp -- tasteful and rare, not spam` / two further cascade failures downstream in the same run, from the same root cause |

Two pixel/state-sampling pitfalls from ADR 0050's own table (sampling a
label's undrawn margin) were checked for and not repeated here: every
`audio_check` assertion reads a counter the recording backend maintains
directly, not a rendered pixel, so there is no analogous "reads background
either way" failure mode to guard against.

## What is unverified

**Nothing in `esp_audio.cpp` has been run.** The ESP-IDF cross-compile
(`idf.py build` against `ports/esp32`, default `KF_PANEL=ili9341`) is
clean — zero warnings or errors anywhere in the build, including the new
file. Firmware image: **557,936 bytes** (65% of the 1,572,864-byte app
partition free). This number is **not** cleanly comparable to ADR 0050's
526,448-byte figure as an "audio costs N bytes" delta — several unrelated
tasks (KFDBG CLOCK/RTC, the drowsy window, overnight floors, the home
clock, sleep-state parity) landed on this branch between that ADR and this
one, and isolating just this task's contribution would have meant
reverting every other agent's uncommitted work-in-progress across the tree
to get a clean "before" build, which is not a safe thing to do tonight.
Reported honestly as the current total rather than a fabricated delta.

**What "compiles clean" does and does not prove:** I2S has no
acknowledgement the way I2C does — `i2s_new_channel()`/
`i2s_channel_init_std_mode()`/`i2s_channel_enable()` all succeed whether or
not anything is actually wired to BCLK/WS/DOUT, unlike `esp_time.cpp`'s
DS3231 driver, which can at least detect "nothing answered." A board with
no amp soldered in will run this driver's entire init sequence
successfully and then produce no sound at all, silently, because there is
nothing on the other end of the wire. That is the correct, safe behaviour
this HAL's "silence is the safe default" contract calls for — but it is
not the same as having heard a tone come out of a speaker, and this task
did not claim otherwise anywhere in the code or in this ADR.

## What Chris needs to solder to hear this on hardware

Three wires, from the MAX98357A breakout to the three already-reserved,
never-wired I2S pins in `kf_esp_pins.h`:

| MAX98357A pin | ESP32-S3 GPIO |
|---|---|
| BCLK | GPIO1 |
| LRC (WS) | GPIO2 |
| DIN | GPIO9 |

Plus power (3V/VIN) and ground, and a speaker on the amp's own output
terminals — the four 1W 8Ω 28mm speakers already on hand. The passive
buzzer modules are not needed for this path at all: `kf_esp_pins.h`'s own
"redundant with the amplifier" reasoning holds, and this task's tone
synthesis runs over the amp, in software, rather than a dedicated buzzer
GPIO. `KF_ESP_PIN_I2S_DIN` (GPIO47, for the INMP441 microphone) is unused
by this task — nothing in this slice reads audio in, only plays it out.

## What changed in the plan

No plan document exists for this task — it was dispatched directly against
ADR 0050's own "what defers to hardware" section, which already named the
exact call site and the exact gap. Nothing to reconcile.
