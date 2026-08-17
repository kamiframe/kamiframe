# Writing the Lua SDK surface

A short design note, not an ADR: this names a principle rather than
recording a decision with alternatives that were rejected. Written down
2026-08-05 at Chris's request, but it is not new -- it describes what
`pet.*` (ADR 0016, extended by ADR 0021) already does. The point of writing
it down is so every binding that gets added after this one is designed
against it on purpose, not by accident.

## The goal

Someone who has never written Lua, and does not think of themselves as a
programmer, should be able to open the demo creature script
(`examples/creature_demo/creature.lua`) and guess what most lines do just by
reading them. `pet.feed()` and `pet.hunger()` already pass that test. The
model here is the same one WordPress and jQuery succeed on: a small,
well-named function library that reads like plain English, sitting on top
of a real, fully powerful language rather than a stripped-down one.
Arduino's `digitalWrite()`/`analogRead()`, PICO-8's tiny built-in API, and
the Playdate SDK (the closest real sibling to Kamiframe -- a small Lua
handheld aimed at hobbyists) are the same idea applied to hardware and
games specifically, and are proof this is not a hypothetical strategy for
a product shaped like this one.

## The rule

Every `kf.*`/`pet.*`-style binding is a small number of clearly-named,
single-purpose functions or methods. Prefer a plain verb a non-expert would
guess correctly (`feed`, `play`, `stage`) over a generic, overloaded entry
point (no `pet.call("feed")`, no `pet.set("hunger", value)`). `pet.*` stays
flat global functions (`pet.feed()`, `pet.hunger()`); the drawing and screen
API (`kf.sprite()`, `kf.screen()`, ADR 0044) returns an **object with
methods** instead (`sprite:move(x, y)`, `screen:show()`) because those
objects are stateful things you keep a handle to and act on repeatedly, not
one-shot queries — the same rule (plain verb, single purpose, no generic
dispatch) applies to the method name either way. If a value needs a name
rather than a number where one actually exists yet, return the name
(`pet.stage()` returns `"baby"`, not the raw enum integer) -- see "What this
does not mean" below for the limits of that. Consistency matters more than
cleverness: once a shape exists (`pet.hunger()`, `pet.happiness()`,
`pet.energy()`), the next value follows the same shape without a special
case.

## The wrapper/raw-power question, answered

Unlike WordPress -- where template tags (PHP functions) and "real PHP" are
the same language, so there is no separate mode to design -- Lua does not
split that way either, and for the identical reason: `pet.*`/`kf.*` are
just Lua functions. There is no restricted subset a script is confined to
and a separate "advanced mode" to unlock. The demo creature script's own
`classify()`/`announce()` helper functions and `bands` table are already
real code, written using the simple API, sitting right next to it in the
same file. Full Lua is not a future feature to add on top of the simple
bindings; it is already there, automatically, because embedding a real
language rather than a restricted DSL was already decided (ADR 0014). The
sandboxing that exists (`io`/`os`/`package`/`debug` left out) is there for
security and portability, not to hold back anyone who wants to write more
than a beginner would -- see ADR 0014's own reasoning for exactly why each
of those four was excluded.

## What this does not mean

Core stays generic; the binding owns the vocabulary, but never invents
content. `pet.teen_form()` and `pet.adult_branch()` return raw indices, not
invented names. The branches themselves are named now -- every entity has a
`display_name` in `tools/character_manifest.toml` (Marumaru, Hamaru,
Chokimaru, and the rest) -- but that naming lives in the demo creature's own
art and manifest data, not in Core or in this binding, and returning an
index rather than a name here is still correct: a wrapper function being
"beginner-friendly" is about how easy the mechanism is to call, not a
license for Core or the binding to make up or hardcode names, characters, or
behaviour that belongs to a specific creature's content instead. That line
stays exactly where `kf/pet.h`'s header comment already draws it.

## What this is not, yet

This is a principle to design against, not a finished onboarding
experience. Real beginner-facing polish -- a tutorial, a cheatsheet, example
cartridges someone else can open and learn from, friendlier error
messages -- needs an actual place for a second person to load their own
script, which does not exist yet: today there is exactly one hardcoded
demo script the simulator loads directly (`sdl_main.cpp`), no cartridge
format, no loading mechanism. That work has its natural home once the
cartridge/public-SDK effort actually starts (see the platform notes' "Still
open" list), not before -- building tutorial-grade polish for an audience
that cannot reach it yet would be work with nowhere to land.

## Your screen's object budget is yours alone

`KF_SCENE_MAX_OBJECTS` (64, `kf/scene.h`) is the number of sprites, text
objects and boxes that can exist at once. It is a **per-screen** budget,
not one shared across your whole game: a `kf.screen()` whose objects are
not on the panel holds none of it, and gets them all back when the player
navigates to it (ADR 0061). So a game with five screens of forty objects
each is fine; one screen wanting sixty-five is not.

Two consequences worth knowing:

- **Declare freely, at the top of your script, in the obvious place.** A
  screen nobody has looked at yet has not taken a slot. You do not need to
  create objects lazily or tear them down by hand — handles you hold stay
  valid across navigation whether or not the screen is showing, and a
  setter called on a screen that is currently off-panel takes effect the
  next time it comes up.
- **Objects created with bare `kf.sprite()`/`kf.text()`/`kf.box()`, rather
  than through a `kf.screen()`, belong to no screen and are never
  released.** That is the right choice for something that must survive
  navigation — an error banner, a global overlay — and the wrong one for
  anything belonging to a single screen, which should be declared through
  that screen so it can hand its slots back.

## The one button that is not the game's

`kf.on_button("menu", ...)` compiles and looks like every other button
binding, but do not use it: MENU is consumed by the screen navigation
registry (`kf_screen_nav.cpp`) to cycle Home -> Info -> Settings -> Home,
and nothing stops a script from binding it anyway on top of that -- see
ADR 0047 for the mechanism. A handler that fires on every screen change,
regardless of which screen is showing, is a bad first experience and not
an obvious one to debug from the script side. Every other button
(A/B/UP/DOWN/LEFT/RIGHT) is the game's to bind freely.

## Summary

- Small, guessable, single-purpose functions, not generic dispatch.
- This is already how `pet.*` was built; the rule just makes it deliberate
  for everything that comes after it.
- Full Lua underneath is not a decision to make -- it is already true,
  because the SDK embeds a real language rather than a restricted one.
- Wrapper functions expose mechanism (how to call something); they never
  invent content (what something is called, looks like, or means) -- that
  boundary is Core's and stays exactly where ADR 0021 already drew it.
- Tutorial/example polish waits for real cartridge loading to exist.
