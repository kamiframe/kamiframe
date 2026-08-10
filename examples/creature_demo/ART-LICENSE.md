# This directory's art is not Apache-2.0

Everything else in this repository defaults to Apache License 2.0 (see the
repository root `LICENSE`). This directory is the one place that default
does not apply to everything in it.

`creature.lua` in this same directory is code, and it is Apache-2.0, same
as the rest of the software -- read it, copy it, build on it.

**`assets.kfpack`, and every file under `sprites/`, is not.** The creature's
characters, artwork, and animations are **Copyright the Kamiframe
contributors, all rights reserved.** Look at them, learn from them, don't
ship them. Don't use them as your own creature, don't sell merchandise with
them, don't bundle them with your own cartridge.

See `LICENSING.md` at the repository root, "The demo creature: split
license", for the full explanation and the reasoning behind the split. The
short version: the engine and the worked example of how to write for it are
free to build on; this one creature's specific look is not, the same way id
Software open-sourced the Doom engine but not the Doom monsters.

If you want a creature of your own, `creature.lua` names sprites by string
(`kf.sprite("baby_neutral_s")`) rather than embedding pixels -- point it at
your own `.kfpack` with matching entry names and it draws your art instead
of this one's. That's the whole mechanism, and it's deliberate: copying the
code gets you no pixels.

Questions about where the line is: open an issue and ask. The root
`LICENSING.md` says the same thing and means it.
