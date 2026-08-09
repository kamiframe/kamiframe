# Regenerating a creature's art, from scratch, with no prior context

This is for whoever has to redo a creature's art someday and has never
touched this tooling before. It assumes nothing except that you can run a
command in a terminal.

There are three separate steps, and they're separate on purpose: writing
down what should exist, generating pictures of it, and turning those
pictures into the one file the device actually loads. You can redo any one
of them without touching the other two.

## The three files that matter

- **`tools/character_manifest.toml`** -- the list. Every creature, every
  pose, every size. Nothing else in this pipeline invents anything; if it's
  not in here, it doesn't get made.
- **A folder of PNG files** -- the pictures. One per row in the list,
  produced by hand, by an artist, or eventually by an AI image generator.
- **A `.kfpack` file** -- the one thing the device (or the desktop
  simulator) actually reads. It's every picture from the folder above,
  glued together into a single binary file the firmware can load in one
  shot, the way a `.zip` bundles up a folder of files into one.

## Step 1: see what's needed

```
python3 tools/kf_character_manifest.py stats
```

This prints how many sprites exist in the list right now, broken down by
life stage, and flags anything that would break (mainly: a name that's too
long -- explained below). Nothing here talks to the network or writes a
file; it just reads the list.

To see the exact filenames expected for one creature:

```
python3 tools/kf_character_manifest.py list --id chokimaru
```

That's the naming convention: `<stage-token><branch-indices>_<pose>_<dir>
[_grudge]_<frame number>` -- generic, not `<creature>_...`, because no real
creature name is allowed to appear in a filename the code loads (every name
in this manifest is an unverified trademark placeholder -- see
`docs/sdk-style-guide.md`). `chokimaru`'s `id` still drives its *prompt*
(see Step 2), but the file the runtime actually asks for is named after its
generic stage and branch position instead -- e.g. `adult00_happy_s_01.png`,
not `chokimaru_happy_01.png`. `hakoniwaos/src/creature.cpp`'s
`kf_creature_sprite_name()` is the authority on this scheme, not this
manifest -- see `tools/kf_character_manifest.py`'s own module docstring
("THE NAMING CONVENTION") for the full breakdown of every token, including
why `_grudge_` sits where it does even though no runtime lookup asks for it
yet.

## Step 2: get a prompt for the image generator

```
python3 tools/kf_prompt_builder.py --id chokimaru
```

This prints a full text description for every pose Chokimaru needs --
object, personality, the exact body-language note for that pose, and the
house art style (Tamagotchi-style pixel art, one exaggerated feature, no
face expressions, transparent background) that every creature shares. You can
read this yourself, hand it to an artist, or eventually paste it into an
image generator.

Useful filters: `--stage adult` (only the ten grown creatures), `--state
happy` (only one pose across everyone), or `--sprite chokimaru_happy_01`
(exactly one file's prompt).

**If a prompt says "NO DESIGN BRIEF YET"** -- that's not a bug. Two of the
five life stages (the very first "egg" and "baby" stages, before a
creature has picked what it will grow into) aren't described anywhere yet.
The tool refuses to make something up for them rather than guess. Someone
needs to decide what those look like before art can be generated for them.

## Step 3: generate the actual pictures

This is the one step that doesn't work yet, on purpose. The plan is to use
an AI image-generation service (Pixellab) to turn each prompt from Step 2
into a picture automatically, but that connection isn't wired up in this
environment. Trying it tells you so, in plain language:

```
python3 tools/kf_generate_sprites.py -o some_folder --id chokimaru
```

Until that's connected, get the pictures however you can -- draw them,
commission them, generate them by hand one at a time -- and save each one
using the exact filename Step 1 gave you, into one folder. Every picture
must have a properly transparent background (not a solid color -- an
actual alpha channel, the same kind of transparency a PNG sticker uses).
That's what Step 4 checks for.

## Step 4: check the pictures and build the real file

```
python3 tools/kf_ingest_sprites.py some_folder -o build/roster.kfpack
```

This looks at every PNG in `some_folder`, checks it against the list from
Step 1 (right size in pixels, actually transparent where the background
should be), and reports anything wrong -- a missing file, a picture that's
the wrong dimensions, one where the artist forgot to remove the
background. Nothing invalid gets packed silently.

If everything checks out, it writes `build/roster.kfpack` and then
independently re-reads that exact file to confirm it comes back out
correctly -- so "it wrote a file" and "the file is actually valid" are two
different, both-checked things.

You don't need every creature ready at once. Point it at a folder with
just a few pictures in it (`--id chokimaru` narrows the check to one
creature) and it will happily pack just those, while still telling you
what's still missing.

## Seeing it in the simulator

Building a pack this way does not change what `kamiframe-sim` shows by
default -- the compiled-in default stays `examples/hello_sprite/assets.kfpack`
(the `KF_ASSET_PACK` CMake cache variable), and every ctest target keeps
checksumming output against that one, untouched. To look at a *different*
pack without recompiling or touching that default, pass it at the command
line:

```
build/kamiframe-sim --pack examples/creature_demo/assets.kfpack
```

`examples/creature_demo/` is the first roster slice actually produced this
way (egg + baby, every state, all three directions). Its `sprites/`
directory holds the 18 source PNGs (48x48, RGBA); `assets.kfpack` is the
packed result. How they were made:

- **Egg** (`create_1_direction_object`, no skeleton) came back exactly
  48x48, no resizing needed. `create_character`'s default humanoid
  skeleton fights a limbless, faceless egg outright, so the egg used a
  different Pixellab tool than the baby did.
- **Baby** (`create_character` for the base `neutral` pose,
  `create_character_state` for the other four states, so every pose stays
  visually the same creature) came back 68x68 -- Pixellab's `size`
  parameter sizes the character, not the canvas, which ships ~40% larger
  "for room to animate." Downscaled to 48x48 with a uniform resize
  (`sips -z 48 48`), not a crop: a crop risks clipping a pose that leans
  or bounces off-centre (`happy`, for one); a resize cannot lose any part
  of the character.
- **The three egg sprites (`egg_idle_s_01`/`_e_01`/`_n_01`) are the exact
  same image file**, deliberately, not a missing-art gap: the egg's own
  design brief says it has no markings and nothing hints at what's
  inside, so a plain, rotationally-symmetric egg genuinely looks identical
  from the front, side, and back. A second or third generation would show
  nothing a viewer couldn't already see in the first.

Nothing about `--pack` is specific to that one pack; point it at any
`.kfpack` a run of `kf_ingest_sprites.py -o` produced.

## If something's confusing

Every one of these commands accepts `--help` and will explain its own
options. If a name in the list looks wrong, don't fix it yourself --
**every character name in this project is an unverified placeholder**
(see `character_manifest.toml`'s own top comment) until it's been checked
for trademark conflicts, which hasn't happened yet.
