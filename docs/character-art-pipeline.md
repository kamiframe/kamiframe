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
packed result.

**Second pass (2026-08-09):** the first pass's art read as thin and
Western-illustration rather than the intended Japanese kawaii look, and the
egg had no markings. Both were regenerated. The kawaii/chubby/blobby
direction is now a standing house-style rule -- see
`tools/kf_prompt_builder.py`'s `KAWAII_SHAPE_BLOCK` -- not a one-batch
prompt tweak, so it applies to every future creature too. How the second
pass was made:

- **Both egg and baby switched to `create_8_direction_object`**, not
  `create_character`. The first pass used `create_character` for the baby
  because it was the tool that gives multiple directions from one
  identity-preserving base; the problem is that `create_character` always
  imposes a humanoid skeleton, even for a body type with no arms or legs,
  and the first-pass baby shipped with a faint limb artifact as a result.
  `create_8_direction_object` has no skeleton at all -- it is built for
  *objects* rendered from 8 angles, not rigged characters -- and a floating
  limbless blob is much closer to an object than a humanoid, so it fits
  better on every axis: no skeleton to fight, and it still gives multiple
  consistent-identity directions the way `create_1_direction_object` (the
  first pass's egg tool) cannot. The trade-off: object generation has no
  humanoid-specific controls (proportions, body type), which this brief
  never needed anyway.
- **State variants use `create_object_state`**, the object-pipeline
  equivalent of `create_character_state` -- it preserves the source
  object's identity and edits pose/expression on top, across all 8
  directions at once. Used for the baby's `happy`/`objecting`/`sick`/
  `sleeping` states, each built from the `neutral` base object.
- **Limblessness was fully achievable this way.** The `create_8_direction_object`
  baby has no arms, legs, or ground contact in any of the three shipped
  directions (`s`/`e`/`n`) or the five unshipped ones. One early attempt
  did grow an unwanted spiral/tail shape on its back and west views --
  traced to the house style's "one exaggerated feature" + "something
  sticking out of the top" asymmetry instructions being satisfied with an
  invented appendage instead of the face -- fixed by pinning the
  exaggerated-feature budget explicitly to the eyes in the prompt and
  regenerating; the final art has no appendage anywhere.
- **The egg came back at the full 48x48 request size** but Chris asked for
  it to read as *smaller* within its frame -- roughly a third the size,
  centred, with transparent margin, canvas unchanged. That's a framing
  instruction, not something to ask the generator to draw directly (asking
  an image model to leave 2/3 of a small canvas blank tends to produce
  worse art than drawing the subject at full confidence and shrinking it
  after), so it's done as a post-process: crop to the drawn egg's own
  bounding box, nearest-neighbour scale that down to ~16px, then pad onto
  a transparent 48x48 canvas, centred. `tools/character_manifest.toml`'s
  new `render_note` field on the egg entry records this instruction where
  the next person would look for it; the crop/scale/pad step itself is
  scratch tooling, not part of the shipped pipeline, since it is a one-off
  post-process rather than something every future sprite needs.
- **Baby's 68x68 canvas** (Pixellab's `size` parameter sizes the drawn
  character, not the canvas, which ships larger "for room to animate," a
  behaviour object generation shares with character generation) is
  downscaled to 48x48 with a uniform nearest-neighbour resize, no crop --
  a crop risks clipping a pose that leans or bounces off-centre (`happy`,
  for one); a resize cannot lose any part of the character.
- **The three egg sprites are no longer identical.** The first pass's
  plain, markingless egg looked genuinely the same from every angle, so
  reusing one file for `_s_01`/`_e_01`/`_n_01` cost nothing. Now that the
  egg has its own speckle pattern, each direction is its own generation --
  a rotationally-consistent but not pixel-identical egg is part of what
  "has its own markings" means.

Nothing about `--pack` is specific to that one pack; point it at any
`.kfpack` a run of `kf_ingest_sprites.py -o` produced.

## If something's confusing

Every one of these commands accepts `--help` and will explain its own
options. If a name in the list looks wrong, don't fix it yourself --
**every character name in this project is an unverified placeholder**
(see `character_manifest.toml`'s own top comment) until it's been checked
for trademark conflicts, which hasn't happened yet.
