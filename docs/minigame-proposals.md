# Minigame proposals

Researched overnight, 2026-08-12. Everything hardware-related below is checked
against `ports/esp32/hal/kf_esp_pins.h` (the pin map) and `kf/budget.h` (the
constraint numbers), not assumed from the Amazon order list — where the two
disagreed, the pin file won. Everything platform-related is checked against
`kf/scene.h`, `docs/frame-budget.md`, `docs/sdk-style-guide.md` and
`examples/creature_demo/creature.lua` — the actual engine and the actual
Lua API a third-party dev would have to write against, not an idealized one.

## The 20-second version

Build the **button memory game** first — it needs no new hardware at all and
proves the "minigame" plumbing works before anything riskier. Then build the
**tilt marble game** you're already excited about — it's genuinely feasible:
you have the accelerometer, the pins for it exist and aren't spoken for, and
the physics can be plain floating-point Lua because only `hakoniwaos/`
itself (the C++ core) is required to stay integer-only, not game scripts.
The mic/speaker rhythm game and the light-sensor peekaboo game are real but
smaller bets. The passive buzzer has no GPIO pin at all — it's dead, don't
design around it. The 2.8" touch panel is a genuine opportunity but splits
the product against your 2" primary panel, so that's a decision for you, not
a recommendation from me. Full detail below.

---

## Hardware reality check

Your Amazon list, checked against what's actually wired and coded today.

| Part | Bus / pins | HAL driver exists? | Verdict |
|---|---|---|---|
| MPU-6050 (accel+gyro) | I2C, `GPIO13`/`GPIO14` — already routed | No | **Usable.** Bus exists, pins exist, needs a new driver (moderate scope, same shape as the DS3231 clock driver already in the tree). Shares address `0x68` with the DS3231 RTC — the MPU's `AD0` pin has to be pulled high to move it to `0x69`, or the two can't coexist on the bus. |
| BH1750 (light) | I2C, same bus | No | **Usable**, same story — new driver needed, but the simplest of the three I2C sensors (single-register lux read). |
| DRV2605L (haptic) | I2C, same bus | No | **Usable** as a feel-enhancer for any game (a bump when the marble hits a wall, a buzz on a correct answer), not really a game on its own. Has a real effect library (100+ canned haptic patterns), not just on/off. |
| BME280 (pressure/temp/humidity) | I2C, same bus | No | Usable, but see "Cut" below — not real-time enough for a minigame. |
| INMP441 (mic) | I2S, `GPIO1`/`GPIO2` (shared clock) + `GPIO47` | No | **Usable**, bigger lift — I2S audio input has no driver yet at all. |
| MAX98357A (amp) + 8Ω speaker | I2S, shares `GPIO1`/`GPIO2` + `GPIO9` | No | Same story as the mic; the two share clock lines but have independent data lines, so both can run at once (mic listening while the amp plays a tone). |
| Passive buzzer | — | — | **No pin was ever given to it.** `kf_esp_pins.h`'s own comment: "the passive buzzer is then redundant with the amplifier and is not given a pin at all." A design that needs the buzzer is not a proposal, it's a wish — see "Cut." |
| ESP32-S3 WiFi/BLE/ESP-NOW | on-chip radio, no extra pins | Not wired into game logic yet | **You own three boards.** Device-to-device play is a real option, not hypothetical — see the ambitious section. |
| ILI9341 2.8" touch panel | touch controller pins **not in the pin map at all** | No | See the callout below. Real capability, real product-split problem. |

**The pin budget is nearly gone.** The file's own accounting: 23 GPIOs are
usable on this board at all (the rest are flash, PSRAM, USB, UART, strapping
pins, or the onboard LED — wiring any of those is a silent brick, not a
loud error). 19 are already spoken for. The 4 that remain are already
earmarked for the mic/amp I2S lines. **That means the I2C sensors
(accelerometer, light, haptic, pressure) are free to use — the bus already
exists — but anything needing its own new dedicated pins, like a touch
controller, has nothing left to claim.** That's not a guess about future
crowding; it's what's left today, before touch is even in the conversation.

### Two things the repo doesn't account for yet

**1. The 2.8" panel has touch. `CLAUDE.md`'s target-hardware list doesn't
mention touch at all**, and the pin map has no touch-controller wiring. A
touch-driven game would be a genuinely good fit for some ideas below (trace
a shape, tap-the-target), but the 2" panel — your stated primary — has no
touch, and there's no free pin to add a touch controller to the board as
currently laid out even if you wanted to. Building a touch-only game
means either accepting it only runs on the 2.8" variant, or resourcing a
non-touch equivalent for the 2" panel and maintaining two input paths for
one game. **That's a product decision, not something I'll resolve by
picking one for you.** I'm flagging it and moving on.

**2. You own three ESP32-S3 boards.** That's not a spare-parts detail, it's
a real capability: ESP-NOW (a lightweight peer-to-peer WiFi protocol, no
router needed — think two walkie-talkies, not two laptops joining the same
Wi-Fi) or BLE could let two Kamiframe devices actually interact. Nothing in
the codebase uses this yet. It's real, but it's also the biggest lift of
anything in this document — see the ambitious section.

---

## The constraints these games live inside

Translated, not just cited — the numbers matter for what's actually cheap.

**The screen is the bottleneck, not the CPU.** `docs/frame-budget.md`
measured this on real hardware: sending one full 240×320 frame over the
SPI wire (the serial connection to the screen — one bit at a time, like a
very fast garden hose instead of a wide pipe) takes about 30.7ms at the
current 40MHz clock, versus ~1.3ms to actually *draw* the frame. Drawing is
4% of your budget; the wire is 96%. That means: **a game where most of the
screen changes every frame — a maze with a moving ball, tiles scrolling by —
still comfortably hits the 30fps target** (a full-screen redraw costs about
31ms, and the target frame budget is 33.3ms), it just can't go faster than
that without hardware changes that are out of scope here. What you *don't*
get for free is 60fps on anything full-screen; that needs a faster
SPI clock or a different panel interface, both hardware decisions already
flagged elsewhere in this repo, not something a minigame's software can fix.

The "dirty rectangle" system (`kf/scene.h`, `KF_MAX_DIRTY_RECTS = 8`) is an
optimization for quiet screens, not a limit on busy ones — a game that
changes everything just repaints everything, same as any full-screen game
would. Where it bites: past 8 separately-changed regions in one frame, the
engine gives up tracking them individually and just redraws the whole
screen — which, per the above, is not disastrous, just not free either. A
game with a handful of moving objects (a maze: one ball, half a dozen
fruit, timer text) stays well under that cap most frames.

**Lua games can use ordinary floating-point math — this project's
"integer-only" rule doesn't reach them.** `hakoniwaos/` (the C++ core) is
enforced heap-free and, by convention, float-free — that's a rule about the
firmware's own internals, checked by `tools/check_no_heap.py` for the heap
half. But the Lua VM this project embeds is deliberately built with
`LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` (ADR 0014) — single-precision floats,
matching the S3 chip's real hardware floating-point unit. So a Lua game
script doing marble velocity, friction, and bounce math in ordinary
floating point isn't fighting the platform; it's using exactly what Lua on
this device was built to do well. The float-free rule is real, but it's a
rule about the engine's own guts, not about the games running on top of it.
Worth saying plainly since it's the one constraint in the brief that turned
out not to bind here.

**Games are Lua** (`examples/creature_demo/creature.lua`,
`docs/sdk-style-guide.md`): small, guessable functions
(`pet.feed()`, `kf.button("a")`) sitting on top of real Lua, not a
restricted toy language. `kf.on_button()` and per-frame callbacks
(`on_home_frame(dt_ms)`-style) already exist and are the shape any of these
games would use. **MENU is reserved by the platform for switching screens —
UP/DOWN/LEFT/RIGHT/A/B are the six buttons available to a game.** That
matches what you remembered ("5 for functions, one for menu, one for B") —
checked against `kf_esp_pins.h` (7 physical buttons exist) and the SDK style
guide (MENU is explicitly off-limits to game code).

**Flash headroom is generous; art still costs real money.** The 12MB asset
budget (`kf/budget.h`) is barely touched — the entire demo creature, every
life stage and animation, packs to about 560KB today. None of the games
below are flash-constrained. But every sprite is still something you pay to
generate, so each entry below says what art it actually needs, not just
"some sprites."

**There's no score-to-reward primitive yet.** `pet.feed()`/`pet.play()`/
`pet.rest()`/`pet.bath()` each take a `variation` (0–2, three fixed
outcomes) — there's no existing way to say "you collected 12 fruit, here's
a proportionally bigger reward." For a first version, mapping a game's
score into a bucket (bad/ok/great → variation 0/1/2) needs no new engine
code at all. A continuous score-to-magnitude reward would need a small new
`pet.*` call — real but small, worth deferring until a game actually needs
it.

---

## The games, ranked

### 1. Button memory / Simon-says — build this first

**What it is.** The engine flashes a growing sequence of directions
(UP/DOWN/LEFT/RIGHT, maybe A/B too); the player repeats it back. Classic
Simon, and it's also almost exactly the original 1996 Tamagotchi's own
"guess which way I'll move next" minigame and the discipline-style
challenges Giga Pets used — this isn't a new idea, it's a proven one.

**Why it's fun.** Short, replayable, difficulty scales itself (sequences
get longer), and it rewards attention rather than reflexes, which suits a
pet-check-in device you pick up for thirty seconds.

**Hardware needed.** None beyond the six buttons you already have wired and
working today.

**Cost.** Pins: zero, nothing new. Flash: near zero — the pattern can be
colored boxes (`kf.box()`), no new sprite art required at all if you want a
genuinely free-art option, or a handful of small icons if you want it to
look nicer. Frame budget: trivial — this is a "quiet screen" in
`docs/frame-budget.md`'s own terms, a few objects changing per frame, nowhere
near the 8-rectangle dirty limit.

**Build difficulty.** Low. Pure Lua game logic on top of `kf.on_button()`
and a handful of `kf.box()`/`kf.text()` objects — no new HAL driver, no new
C++ at all. This is also a good candidate for the SDK's own example
library, since it demonstrates the full "declare a screen, react to
buttons, call `pet.play()`" shape a third-party dev would copy.

**What it does NOT need.** No new hardware, no new pins, no new HAL code,
no new flash budget to speak of.

**Why build it first, specifically:** it proves the parts of "a minigame"
that have nothing to do with sensors — entering a minigame from Home,
running its own screen and timer, scoring a result, and turning that score
into `pet.play(variation)` — before spending any time on an IMU driver that
doesn't exist yet. If something about that plumbing needs to change, better
to find out on the free game.

---

### 2. Tilt marble maze — the flagship, build second

**What it is.** Your instinct is right and the precedent is real: Tamagotchi
Uni's "Tama Walk" fruit-collecting minigame puts the pet on a small field
and has the player tilt the device — accelerometer-driven, any direction —
to steer it around collecting items inside a time limit (verified: it's a
30-second round, tilting left/right/forward/back changes direction, and
each fruit collected counts toward a reward). Nintendo's Kirby Tilt 'n'
Tumble (Game Boy Color, 2000) is the deeper precedent for the physics side —
a cartridge with a built-in tilt sensor, rolling Kirby toward a goal,
collecting stars along the way.

**What actually makes the Tama Uni version work**, worth being honest about
since it's the thing to imitate, not just the concept: the sensor only
needs to answer "which way is the device leaning," not precise angle — Kirby
Tilt 'n' Tumble's own sensor was binary-direction only (four directions, no
degree of tilt), and the *game* compensated with generous, forgiving
physics rather than needing sensor precision. The round is short (30
seconds), so hand fatigue and drift never become a problem. And there's
always something on screen to grab, so a player tilting somewhat
incompetently still feels like they're making progress. None of that needs
exotic hardware — it needs restrained scope.

**Kamiframe's version.** A single-screen field (not a scrolling maze — see
frame budget below), a handful of static walls, the pet/marble sprite, and
4–6 fruit/item sprites placed on the field. Tilt steers it; colliding with a
wall stops it; touching an item collects it; a 20–30 second timer ends the
round. Score maps to a `pet.play()` variation the same way any other
minigame would.

**Hardware needed.** MPU-6050 accelerometer. **You have it** — 5 units
ordered, I2C bus already routed (`GPIO13`/`GPIO14`), no new pins required.
Two catches worth flagging honestly: the MPU-6050 answers on the same I2C
address (`0x68`) as the DS3231 RTC you're already using, so its `AD0` pin
needs pulling high to move it to `0x69` before both can be on the bus at
once (same collision `kf_esp_pins.h` and ADR 0026 already document for the
RTC driver — not a new problem, just one this game inherits). And there is
no HAL driver for the IMU at all yet — this is real, scoped work: an I2C
register-read driver (comparable in size to the existing DS3231 driver) plus
a small Lua binding (`kf.tilt_x()`/`kf.tilt_y()`, or similar) exposing
filtered accelerometer values to game scripts.

**Cost.** Pins: zero new (bus exists). Flash: modest — a field background,
a marble/pet sprite (can likely reuse an existing creature pose rather than
drawing a new one), 4–6 small item sprites, a handful of wall tiles. Frame
budget: this is a "busy screen" in `docs/frame-budget.md`'s terms — the
marble moves continuously, so most frames redraw close to the whole screen.
That lands right around the ~31fps ceiling the frame-budget doc measured for
full-screen animation — which meets the 30fps target the platform is
actually built for, it just isn't "free" the way the button game is, and it
will not hit 60fps without hardware changes out of scope here.

**Physics, plainly.** This is where the "float-free core" constraint in the
brief matters and doesn't — the actual game loop (marble velocity, simple
friction, bounce off walls) runs entirely in the Lua game script, not in
`hakoniwaos/` itself, so it's free to use ordinary floating-point math (see
the constraints section above). Keep the physics arcade-simple —
axis-aligned wall boxes, clamped velocity, no rotation or restitution
modeling — both because that's genuinely what makes Kirby Tilt 'n' Tumble
and Tama Uni feel good (forgiving, not simulated) and because it keeps the
object count and per-frame math small enough to stay comfortably inside
`KF_SCENE_MAX_OBJECTS` (64) and the frame budget above.

**Build difficulty.** Medium. The new pieces are a real I2C sensor driver
(one afternoon-to-few-days class of work, going by the DS3231 driver
already in the tree), a Lua binding for it, and then a genuinely new game
screen with its own physics loop — more Lua than the button game, but none
of it is architecturally risky; it fits the existing scene/screen/button
model without needing engine changes.

**What it does NOT need.** No touch, no microphone, no amp, no haptic
driver (haptic bump-on-wall-hit is a nice optional garnish once DRV2605L
support exists, never a requirement), no new pins, no buzzer.

**Why it's the flagship and not the first build:** it's the one you're
excited about and the one that actually differentiates Kamiframe from a
button-only virtual pet — worth building and worth marketing. It just isn't
the *cheapest* place to prove the minigame plumbing works, which is why
Game 1 goes first.

---

### 3. Light peekaboo — a real but smaller bet

**What it is.** A short reaction game built on the BH1750 light sensor: the
screen prompts "cover me!" or "let the light in!", alternating, and the
player has to physically cup a hand over the sensor (or uncover it) within
a beat. Miss the beat, lose a life; hit several in a row, round ends on a
score.

**Why it's fun, honestly.** This one doesn't have the pedigree the tilt
game does — I found no direct precedent of a light-sensor minigame in any
of Tamagotchi, Digimon, Giga Pets, or the Nintendo handheld-sensor era. The
closest relatives are "shake to wake" gestures in modern virtual pets and
plain reaction-timer games generally. It's a genuinely physical, toy-like
interaction (you're covering the device with your hand, which reads as
tactile in a way button presses don't), but it's more speculative than
proven. Rank it as a good third game, not a headline.

**Hardware needed.** BH1750. **You have it** (3 units), I2C bus already
routed, no new pins. Simplest of the three I2C sensor drivers to write — a
single-register lux read, no filtering or calibration subtlety like the IMU
needs.

**Cost.** Pins: zero new. Flash: minimal — a few icon/animation frames,
mostly text and timer UI. Frame budget: cheap, a quiet screen in
`docs/frame-budget.md`'s terms.

**Build difficulty.** Low-medium — the lightest new sensor driver of the
three, plus a small reaction-timer game loop.

**What it does NOT need.** No touch, no mic, no amp, no haptic, no new
pins, no buzzer.

---

### 4. Blow/clap rhythm — real differentiator, bigger lift

**What it is.** A call-and-response rhythm game: the speaker plays a short
tone or beat pattern, and the player has to blow, clap, or shout into the
microphone in time. Not pitch or speech recognition — just "was there a
loud sound near this beat," an amplitude/energy-threshold check, which is
realistic on this hardware; real audio recognition is not. The closest
precedent is Nintendogs' "blow into the mic" novelty and, on a similarly
tiny indie handheld, Playdate titles (Crankstone among others) that mix
accelerometer and microphone input for timing-based play.

**Why it's fun.** Nothing in the Tamagotchi/Digimon/Giga Pets lineage uses
sound input at all — this would be a genuine first for the category at this
price point, not a reskin of an existing mechanic.

**Hardware needed.** INMP441 mic **and** MAX98357A amp + speaker, both of
which you have (5 mics, 2 amps, 4 speakers). They share I2S clock lines
(`GPIO1`/`GPIO2`) but have independent data lines, so listening and playing
back at once is genuinely possible on this wiring, not a conflict.

**Cost.** Pins: zero new (already reserved, just unwired/undriven). Flash:
modest — a reacting character animation, a few icon states. Frame budget:
cheap on the display side; the real cost is elsewhere (below). **The catch
is driver work, not pins**: I2S audio input has no HAL driver in this
codebase at all yet, and neither does I2S audio output — both need writing
from scratch, which is a meaningfully bigger lift than any I2C sensor
driver here. Keeping the mic side to "loud or not" amplitude detection
(not waveform analysis) keeps this bounded rather than open-ended.

**Build difficulty.** Medium-high — two new HAL drivers (I2S in, I2S out)
before any game logic, though the game logic itself, once the drivers
exist, is comparable to the button game.

**What it does NOT need.** No touch, no new pins, no haptic. Does not need
the buzzer — the amp supersedes it entirely, which is exactly why the
buzzer was never wired in the first place.

---

## Cut

**Anything that needs the passive buzzer as its primary input or output.**
It has no GPIO pin assigned, full stop — `kf_esp_pins.h`'s own words: "the
passive buzzer is then redundant with the amplifier and is not given a pin
at all." A buzzer-driven rhythm game is not a cheaper version of Game 4,
it's a wish for hardware that isn't wired. If you want a rhythm game, it's
the mic/amp version above or nothing.

**A standalone BME280 (pressure/temperature/humidity) minigame.** I looked
for a way to make this real-time and fun and didn't find one worth
proposing. Pressure-as-altitude is a genuinely interesting fact about the
sensor, but altitude, temperature and humidity all change on the timescale
of minutes to hours, not the seconds a minigame round needs — there's no
"react to this" moment to build a game around. It's a better fit as a
passive/ambient feature later (a pet that notices it went up a hill, or
that the room got warmer) than as an entry on this list, and I'd rather cut
it than pad the list with something that isn't actually a game.

---

## Deliberately too ambitious — where the ceiling is

Two ideas, clearly labeled as reach, so you can see past the shortlist
without mistaking either for a real proposal.

**A. Full-physics marble maze with custom levels.** Take Game 2 and push it
much further: real restitution/momentum physics instead of arcade-simple
collision, haptic feedback on every bump (DRV2605L), a light-based mechanic
layered on top (a "torch" that only lights the maze while covered/uncovered),
and a level editor whose output saves to the microSD card so players can
build and share mazes. Every individual piece above is plausible in
isolation; combined, it's three new HAL drivers, a persistence format, and
an editor UI, which is a project on its own, not a minigame. Good aspiration
for what the platform could support in year two, not a next sprint.

**B. ESP-NOW device-to-device duel.** You own three boards, which makes
this concretely possible rather than hypothetical — a real-time tilt race
between two players' devices, or a turn-based Digimon-style battle over the
wireless link. The hard part isn't the radio (ESP-NOW is a lightweight,
router-free peer link, well within an ESP32-S3's normal capability); it's
game design and engineering for two devices whose game state has to agree
with each other over a link that can drop packets, plus needing a second
person who also owns a Kamiframe to play against — a real constraint until
the platform has more than one owner. Worth keeping in mind as the reason
to design the pet-state and scoring model in a way that could later be
synced, not worth building now.

---

## SDK gaps this surfaced

Not blocking any of the above, but worth having on record since they'd need
solving before the games that need them:

- No IMU (MPU-6050) HAL driver or Lua binding exists. Needed for Game 2.
- No BH1750 HAL driver or Lua binding exists. Needed for Game 3.
- No I2S audio input or output HAL driver exists. Needed for Game 4.
- No touch controller is wired or has a HAL driver. Needed for any
  touch-based idea, and blocked on the board decision above regardless.
- `pet.*` care actions reward in three fixed buckets (`variation` 0–2), not
  a continuous score. Fine for every game above if score maps to a bucket;
  would need a small new `pet.*` call if you ever want reward to scale
  smoothly with performance.

---

## Recommendation

**Build order: button memory game first, tilt marble maze second.** The
first proves the minigame plumbing (entering from Home, running a scored
round, paying the result into `pet.play()`) with zero hardware risk and
ships this week. The second is the one worth marketing — it's the idea you
already wanted, it's technically real (you have the sensor, the pins exist,
the physics can live entirely in ordinary Lua floats), and it's the clearest
answer to "why is this more than a button-only Tamagotchi clone." Light
peekaboo and the mic/speaker rhythm game are both real next games after
that, in that order, roughly matching how much new HAL driver work each
needs. The passive buzzer and a standalone BME280 game are cut outright —
one has no pin, the other has no real-time hook. Touch is a genuine
opportunity sitting on a real product-split question about your two panels,
not a proposal I'm making here.

---
---

# Second pass — 2026-08-12, later the same day

A deliberate re-brainstorm against the sensor list rather than against the
four ideas above. Same rules as the first pass: everything below is checked
against `ports/esp32/hal/kf_esp_pins.h`, `sdk/lua/kf_lua_port.cpp` (the
actual Lua surface today), `hakoniwaos/include/kf/hal/` and
`docs/frame-budget.md`. Nothing here needs wireless, IR or Bluetooth —
that's next-revision hardware by your call, so the device-to-device ideas
have been pulled out into their own v2 section at the bottom and are not
mixed into the shortlist.

## Two corrections to the first pass

**1. Sound output already exists. The first pass said it didn't.** Commit
`f5c4714` landed `kf/hal/audio.h` with three backends and `kf.tone(hz, ms)`
/ `kf.beep()` in Lua. That was true when the first pass was written this
morning and isn't now. It changes one real thing: **a rhythm/music game no
longer needs the microphone at all** if the player answers with buttons
instead of claps. Game 4's "medium-high, two new HAL drivers" estimate was
for mic *and* amp; the amp half is done, and a tone-based music game is
buildable today with zero new hardware work. See game 5.

**2. Temperature is nearly free, and the first pass cut it too fast.** The
BME280 was cut for being too slow, which is right. But `esp_time.cpp`
already reads the **DS3231's own temperature register** (`0x11`) on every
boot — it uses it as an identity probe to tell a real RTC from an MPU-6050
answering on the same address. The sensor is on the bus, the driver exists,
the read is written and working. Exposing it to Lua is a small binding, not
a new driver. It's still too slow for a scored round (see game 16), but
"too slow for a round" and "no cost to try" are different verdicts.

## What the first pass under-used

Listing these plainly because most of the ideas below come out of them:

- **The MPU-6050 is a gyroscope too, not just an accelerometer.** The first
  pass only ever used it as a tilt sensor. Angular rate is a separate signal
  and gives you flick, twist, spin and shake as distinct inputs.
- **Stillness is an input.** Every idea above asks the player to *do*
  something. Asking them to hold perfectly still, or stay quiet, is a
  mechanic the hardware reads just as well and nobody in this category uses.
- **The wall clock is a sensor.** `kf.hour()`, `kf.minute()` and `kf.time()`
  are already bound and the RTC survives a power cut (proven on the bench).
  Time-of-day and day-boundary mechanics cost zero hardware.
- **Not every game needs to be a round on a screen.** The step-counter
  lineage (Pokéwalker, the Digivice, Tamagotchi's own pedometer models) is
  accumulation that happens in your pocket. That's arguably the strongest
  fit for a device with a coin-cell-backed clock and a battery.
- **The haptic driver is an output channel nothing has claimed.** A game can
  speak through it in the dark, with the screen off.

---

## New proposals — no new hardware at all

These four need zero driver work. Everything they use is bound in Lua today.

### 5. Melody echo — Simon, but with pitch

**What it is.** The pet sings a short phrase — three or four tones via
`kf.tone()` — and the player plays it back on the buttons, each button
mapped to a pitch. The phrase grows. Same skeleton as game 1, different
sense.

**Why it's worth having as well as game 1.** It's the same proven loop with
a second channel, and it's the cheapest possible proof that the audio HAL
works end to end on real hardware in a way a user would notice. It also
makes the device *audibly* a toy the first time someone picks it up, which
is a marketing fact as much as a design one.

**Sensors.** None. Buttons in, `kf.tone()` out.
**Cost.** Pins zero, flash near zero (boxes and text), quiet screen.
**Difficulty.** Low — lower than game 1, since game 1 will already have
built the enter-a-minigame-and-score-it plumbing this reuses.

### 6. Daily puzzle — one seeded board a day

**What it is.** A small turn-based puzzle — Lights Out on a 4×4 grid, or a
5×5 Sokoban room — where the board is generated from the current date, so
every device shows the same puzzle on the same day and there's exactly one
per day. Solve it, the pet gets its reward; come back tomorrow for a new
one.

**Why it's fun.** This is the Wordle shape, and it's the single highest
content-per-byte idea in this document: infinite distinct boards from a
seed and a generator, no art, no assets, no flash cost. It also gives the
device a reason to be picked up on a day when the pet doesn't need
anything, which is the actual retention problem a virtual pet has.

**Sensors.** The wall clock (`kf.time()`), already bound. Storage for
"solved today" so it can't be farmed.
**Cost.** Pins zero, flash ~zero, and it's the quietest screen in this
document — a handful of boxes changing on a keypress, nowhere near the
8-dirty-rect cap. Frame budget is a non-issue.
**Difficulty.** Low, with one caveat: a puzzle generator that guarantees
solvable boards is real work, and Lights Out is much easier to guarantee
than Sokoban (generate from a solved state by applying random legal moves
backwards). Start with Lights Out.
**Worth knowing:** this is the one game here that would benefit from the
continuous-reward gap in the SDK gaps list — "solved in 6 moves vs 14"
wants to pay differently, and buckets flatten that.

### 7. Ten seconds — the internal-clock game

**What it is.** The screen says "press A when you think ten seconds have
passed," then goes blank. No timer, no counter, no ticking. Score is how
close you got.

**Why it's fun.** It's a party trick, it's over in ten seconds, it's
genuinely hard, and it is completely unlike anything else on the device. It
also has an unfair advantage on hardware like this: a phone can do this but
a phone is full of clocks — this device can actually take every reference
away, and there's a real charm in a toy that asks you to sit still and
count.

**Sensors.** None. One button and the frame timer.
**Cost.** Effectively nothing on every axis. This is the smallest game in
the document by a wide margin.
**Difficulty.** Trivial — an afternoon, most of it spent on the presentation
rather than the logic.

### 8. Reaction grid — whack-a-mole

**What it is.** A 3×3 field; things pop up in cells; D-pad moves a cursor,
A hits. Speed ramps. Miss three, round over.

**Why it's fun.** It's the reflex counterpart to game 1's memory, so the two
together cover both halves of what a thirty-second pick-up game can be. Very
well-proven mechanic, and unlike most reflex games it's readable at 240×320
without any art at all.

**Sensors.** None.
**Cost.** Pins zero, flash near zero (nine boxes and a cursor, or nine small
icons if you want it pretty). Frame budget: this is exactly the workload the
dirty-rectangle system was built for — two or three cells change per frame,
never the whole screen.
**Difficulty.** Low.

---

## New proposals — accelerometer and gyroscope (MPU-6050)

All four share game 2's prerequisite: one new I2C driver plus a Lua binding.
Once that exists, each of these is Lua-only. They're listed in the order I'd
build them, and deliberately span very different feels so the sensor isn't
just "the tilt one."

### 9. Pocket walk — the step counter

**What it is.** Not a screen game. The device counts steps in the background
while it's in a pocket or bag, and the pet earns something for distance
walked — happiness, an item, a growth path that only opens if the pet gets
taken outside. You check in later and see what the day's walking earned.

**Why this is the strongest idea in the second pass.** The precedent is
enormous and specific: the Pokéwalker, the Digivice's step counting, and
Tamagotchi's own pedometer models all built their entire identity on this,
and it is the mechanic that makes a virtual pet a thing you *carry* rather
than a thing on a desk. It also fits this device's actual architecture
better than any minigame does — you already have offline fast-forward,
persistent state and a battery-backed clock, and this is the same shape as
those: something that accrues while you're not looking. And it's the only
idea here that generates engagement without asking for attention.

**Sensors.** Accelerometer only. Step detection at 20–50Hz is undemanding —
magnitude, a low-pass filter, a threshold and a refractory period. No gyro,
no precision.
**Cost.** Pins zero. Flash near zero. Frame budget: none, it doesn't draw.
**The real cost is power, not frames** — this wants the accelerometer
sampling while the screen is off, which interacts directly with the <50µA
deep-sleep target in `CLAUDE.md`. The MPU-6050 has a low-power wake-on-motion
mode for exactly this, but "how much does this cost per day of battery" is a
measurement, not something I'd claim from a datasheet. **Worth measuring
before committing to it.**
**Difficulty.** Medium — the driver is game 2's driver, the step algorithm
is simple, and the awkward part is the sleep/wake integration, not the
counting.

### 10. Steady hand — the stillness game

**What it is.** The pet balances something — a full cup, a stack of blocks,
a sleeping smaller creature — and the player has to hold the device
absolutely still for fifteen seconds while the difficulty ramps (the stack
grows, the pet takes a step). Any wobble past a threshold tips it.

**Why it's fun, and why it's the one I'd build right after game 2.** It's
the exact inverse of the marble maze on the same sensor: game 2 rewards
motion, this rewards its absence, and the two feel completely different in
the hand despite sharing a driver. It's tense in a way a button game can't
be — you can feel yourself failing — and it costs almost nothing to draw.

**Sensors.** Accelerometer (jitter magnitude); the gyro makes it better
still (rotation is easier to feel than translation) but isn't required.
**Cost.** Pins zero. Flash: one prop sprite and a couple of pet poses.
**Frame budget: cheap** — a wobble is a few pixels of movement on a static
background, a quiet screen, not game 2's full-screen redraw. That's a real
advantage over the marble: it's the sensor showcase that runs at any frame
rate you like.
**Difficulty.** Low, once the driver exists. The whole game is one
thresholded number over time.

### 11. Fishing — flick, wait, strike

**What it is.** Flick the device forward to cast (a sharp accelerometer
spike, distance scales with how hard). Then wait — screen mostly still, the
float bobbing. When the pet's line twitches, jerk the device *up* within a
short window to hook it. A short tug-of-war and you land it.

**Why it's fun.** Fishing is a near-universal virtual pet minigame for good
reason: the waiting is the game, and the payoff is a collectable. It also
uses the accelerometer in a genuinely different way from tilting — discrete
gestures with timing, not continuous steering — so it doesn't feel like a
reskin of game 2. And the collectable half hooks straight into whatever
item/collection system a pet game wants later.

**Sensors.** Accelerometer, gesture detection only (spike magnitude and
direction). Much more forgiving than continuous tilt: you're looking for
"was there a jerk, roughly which way," not an angle.
**Cost.** Pins zero. Flash: a water background, a float, a few fish sprites —
this one does want real art, more than anything else in the second pass, and
the fish are the reason to build it. Frame budget: cheap most of the time
(the waiting phase is a quiet screen), briefly busy on a catch.
**Difficulty.** Low-medium after the driver. Gesture thresholds need tuning
by feel on real hardware, which is an evening of fiddling, not a design risk.

### 12. Do What Marumaru Says — Bop It, with everything

**What it is.** The pet shouts an instruction and you have a shrinking window
to obey: **SHAKE ME · TILT LEFT · PRESS B · TURN ME OVER · COVER MY EYES ·
BE QUIET · HOLD STILL**. Speed ramps until you fail. Every sensor on the
board is one command in the pool.

**Why it's the showcase.** This is the game that makes someone say "wait,
what *is* that thing." It's also structurally clever for this project
specifically: **the command pool is a list, and each new driver adds an
entry to it.** Ship it with buttons only, add tilt and shake when the IMU
driver lands, add "cover my eyes" when the light sensor lands, add "be
quiet" when the mic lands. It grows as the platform grows instead of
blocking on the platform being finished. Bop It sold tens of millions of
units on this exact loop; nothing in the virtual pet category has done it.

**Sensors.** All of them, incrementally, and none of them required to start.
**Cost.** Pins zero. Flash: text and a reacting pet, minimal. Frame budget:
quiet — one instruction and a shrinking bar.
**Difficulty.** Low per command, and it's the best possible test harness for
each new sensor driver: if the command works in this game, the driver works.
**The catch worth flagging:** "turn me over" and "shake me" are physically
rough on a breadboarded prototype with jumper wires in it. Fine on a real
enclosure, worth not demoing on the bench build.

---

## New proposals — light, temperature, haptic

### 13. Sunbathing — light as a slow resource

**What it is.** Not a round. The pet gains something from being in bright
light and something different from being in the dark: leave it on a
windowsill in the morning and it charges up; leave it in a drawer and it
gets pale, or sleepy, or nocturnal. A meter that fills over hours, not
seconds.

**Why it's fun.** It makes the device's *physical placement in your home* a
game input, which is exactly the toy-like, non-screen quality the light
peekaboo game (game 3) was reaching for but on a timescale the sensor is
actually good at. Tamagotchi's lineage has light-based sleep behaviour;
nothing has made light a resource you spend.

**Sensors.** BH1750 (same new driver game 3 needs). Pairs naturally with the
wall clock — "bright at 3am" means something different from "bright at 3pm,"
and both readings are available.
**Cost.** Pins zero, flash near zero, no frame cost.
**Difficulty.** Low after the driver, and the driver is the simplest of the
three.

### 14. Hot and cold — the haptic hunt

**What it is.** Something is hidden in orientation space: a treasure exists
at one particular tilt angle, and the device buzzes faster the closer you
get. The screen shows nothing useful. You find it by feel.

**Why it's fun.** It's the only idea here where the screen isn't the primary
output, and the sensation of hunting for something invisible in the air in
front of you is genuinely novel on a device this size. It's also the honest
answer to "what is the haptic driver actually *for*" — bump-on-wall-hit
garnish doesn't justify a driver, this does.

**Sensors.** MPU-6050 in, DRV2605L out — two new drivers, which is why this
ranks below the rest despite being the most interesting.
**Cost.** Pins zero. Flash zero — it deliberately has almost no art.
**Difficulty.** Medium: two drivers, though both are ordinary I2C register
work, and the DRV2605L's canned effect library does the hard part of making
a buzz feel like something.
**Cheaper first version:** the same game with `kf.tone()` instead of haptic —
pitch rises as you get closer. Buildable the moment the IMU driver lands, no
haptic driver needed, and it tells you whether the mechanic is fun before
you pay for the second driver.

### 15. Warm hands — the hug, not a game

**What it is.** Hold the device cupped in both hands, or against you, for
half a minute and the pet notices it got warm. A ritual with a reward, not a
scored round.

**Why it's here despite not being a game.** It's free — the DS3231
temperature read already runs (see correction 2), so this is a Lua binding
and some game logic, no driver at all. And it's the most emotionally direct
interaction in this whole document: physically warming your pet with your
hands is a better idea than most of the scored rounds above.

**Sensors.** DS3231 temperature register, already read today.
**Cost.** Genuinely near zero on every axis.
**Difficulty.** Low, with one honest caveat: **body heat through a plastic
enclosure moves the reading a degree or two over 30–60 seconds, not
instantly**, and the DS3231's temperature register updates on its own
schedule (roughly once a minute in normal operation), which sets a hard
floor on responsiveness. Design it as a slow ritual and that's fine. Design
it as a reaction game and it will fail. **Worth a five-minute measurement on
the bench before building anything on it** — hold the board, watch the
register, see what it actually does.

---

## New proposals — microphone

Both need the I2S input driver that doesn't exist yet (the output half now
does — see correction 1). Both are amplitude-only, no recognition.

### 16. The quiet game

**What it is.** The pet is asleep. Keep the room quiet for sixty seconds.
Any noise above a threshold and it stirs; enough noise and it wakes up
grumpy. That's the whole game.

**Why it's fun.** It's the mic equivalent of game 10 — an inverted goal that
nothing in this category does — and it's genuinely funny in a room with
other people in it. It also pairs perfectly with the sleep behaviour that
already exists in the pet simulation (`pet.asleep()`, `pet.drowsy()`,
`pet.tuck_in()` are all bound today), so it's a minigame that's really a
care action.

**Sensors.** Mic, amplitude only — the least demanding possible use of it.
**Cost.** Pins zero (reserved already). Flash: a sleeping pet animation you
probably already have. Frame budget: nothing.
**Difficulty.** Medium — entirely the I2S input driver. The game on top is
an evening.

### 17. Blow to fly

**What it is.** Blow steadily into the mic to keep the pet airborne — a
balloon, a leaf, a paper glider — over obstacles. Stop blowing and it sinks.

**Why it's fun, and why it's mechanically distinct.** Sustained blowing is
the only **continuous analog input** the device has other than tilt. A
button is on or off; blowing has a magnitude you modulate with your breath,
which makes for a completely different feel — the Flappy Bird loop, but
controlled by lung pressure. Nintendogs proved blowing into a mic reads as
delightful rather than gimmicky.

**Sensors.** Mic amplitude. Note that blowing directly at a MEMS mic
produces a large low-frequency signal — easy to detect, and easy to
accidentally saturate, so this wants a bit of tuning.
**Cost.** Pins zero. Flash: a pet pose plus simple obstacles. Frame budget:
this is a scrolling game, so it's a busy screen like game 2 — ~30fps
ceiling, which is fine.
**Difficulty.** Medium-high, same as game 4 and for the same reason: the
driver.

---

## Added to the cut list

**Toss and catch.** The MPU-6050 can detect free-fall, and "throw your pet
in the air" is an obviously fun idea. It is also a game whose failure mode
is your prototype hitting the floor, and whose success mode teaches people
to throw the thing. Cut on product grounds, not technical ones.

**Anything needing a compass or heading.** The MPU-6050 is a 6-axis part —
accelerometer and gyro, **no magnetometer.** Treasure hunts that need "walk
north," or anything that wants an absolute heading, have no sensor on this
board. Yaw can be integrated from the gyro but drifts within seconds, which
is not a heading. Flagging it because it's the kind of thing that sounds
available and isn't.

**Screen-tap-by-shock.** It's tempting to use the accelerometer to detect
tapping the screen and get pseudo-touch without a touch controller. It does
technically work, but it's a single noisy binary event with no position,
which isn't touch — it's one more button, and you have seven. Not worth a
driver's worth of tuning.

---

## Revised build order

Slotting the new ideas into the first pass's order, cheapest-proof-first:

1. **Button memory** (game 1) — unchanged, still first, still the plumbing proof.
2. **Melody echo** (game 5) — near-free once game 1 exists, proves the audio HAL to a user.
3. **Ten seconds** (game 7) and **reaction grid** (game 8) — filler in the best sense; a day each, and they make the device feel like it has a games *list* rather than a game.
4. **Tilt marble maze** (game 2) — the flagship, and the thing that pays for the IMU driver.
5. **Steady hand** (game 10) — immediately after, because it's nearly free once the driver exists and feels nothing like game 2.
6. **Pocket walk** (game 9) — the highest-value idea in the second pass, but measure the battery cost before committing.
7. **Daily puzzle** (game 6) — whenever the retention problem starts mattering more than the novelty one.
8. **Do What Marumaru Says** (game 12) — start it early as a button-only game and let it grow with each driver; it doubles as the driver test harness.

Light, temperature, haptic and mic games follow their drivers, in the order
the first pass already gave.

---
---

# v2 — battling and device-to-device

Explicitly out of scope for v1 hardware, gathered here so nothing above has
to pretend it isn't interesting. Wireless, IR and Bluetooth are all
next-revision by your call; the ideas below assume some link exists without
caring which.

**One thing worth deciding early even though it's a v2 feature:** whether
pet state is *designed* to be syncable. A battle needs an agreed,
serialisable description of a creature — stats, form, name, a checksum. If
the save format grows up without that in mind, v2 pays to retrofit it. The
first pass already flagged this at the bottom of its ambitious section; it's
the one v2 concern that has a v1 cost if ignored.

**A. Turn-based duel.** The Digimon model: two devices link, each pet has
stats derived from how it was raised, and a short turn-based exchange
resolves a winner. The interesting part isn't the combat, it's that the
combat *reads your parenting back to you* — a pet raised on the step counter
fights differently from one raised on puzzles. That connection is what makes
a battle mean something, and it's designable now even though the link isn't.

**B. Tilt duel.** Real-time, both players tilting: a tug-of-war where the
contested object's position is the sum of two devices' tilt, or a parry game
where you have to tilt the opposite way to the incoming attack within a
window. Uses game 2's driver on both ends. The hard part is that real-time
state agreement over a lossy link is a genuinely difficult engineering
problem, and much harder than turn-based.

**C. Rhythm duel.** Both devices play the same beat through their amps and
both players answer; closest to the beat wins the exchange. Only needs to
sync a clock and a beat pattern, not continuous state, which makes it the
*easiest* real-time option by a wide margin — worth remembering if B looks
too expensive.

**D. Asynchronous ghost battles.** Each device exports its pet as a small
"challenger" blob; you fight the recorded ghost of someone else's pet
rather than a live opponent. **This is the one that could work without any
radio at all** — a short alphanumeric code entered on the buttons, or a file
passed on the microSD card, both of which v1 already has. Worth noting
because it solves the problem the first pass identified as the real blocker
on device-to-device play: needing a second person who owns a Kamiframe *at
the same moment you do*. A ghost doesn't have to be online.

**E. Co-op rather than versus.** Two devices, one shared goal — carry
something between them (both must hold still, game 10 doubled), or a call
and response where one device shows the pattern and the other has to enter
it. Under-explored generally, and a better fit for a gentle pet device than
combat is.

**F. Trading and breeding.** Not a battle, but the same plumbing: exchange
creatures or traits between devices. The longest-shadow feature in this
list, because it constrains the save format, the identity model and the
content pipeline all at once — which is exactly why it's worth naming now
even though it's years out.
