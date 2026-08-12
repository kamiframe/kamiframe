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
