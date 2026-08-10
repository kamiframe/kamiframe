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
single-purpose global functions. Prefer a plain verb a non-expert would
guess correctly (`feed`, `play`, `stage`) over a generic, overloaded entry
point (no `pet.call("feed")`, no `pet.set("hunger", value)`). If a value
needs a name rather than a number where one actually exists yet, return
the name (`pet.stage()` returns `"baby"`, not the raw enum integer) --
see "What this does not mean" below for the limits of that. Consistency
matters more than cleverness: once a shape exists (`pet.hunger()`,
`pet.happiness()`, `pet.energy()`), the next value follows the same shape
without a special case.

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
invented names, because naming those branches is real creative content
that does not exist yet (ADR 0021) -- a wrapper function being
"beginner-friendly" is about how easy the mechanism is to call, not a
license to make up names, characters, or behaviour on Core's or the
binding's own authority. That line stays exactly where `kf/pet.h`'s header
comment already draws it.

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
