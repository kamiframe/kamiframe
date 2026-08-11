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

That's the naming convention, and it has two forms: `<stage-token><branch-
indices>_<pose>_<dir>[_grudge]` for the thing the *runtime* asks for, and
the same string plus `_<frame number>.png` for the *picture files* on disk.
Both generic, not `<creature>_...`, because no real creature name is
allowed to appear anywhere the code loads (every name in this manifest is
an unverified trademark placeholder -- see `docs/sdk-style-guide.md`).
`chokimaru`'s `id` still drives its *prompt* (see Step 2), but its picture
files are named after generic stage and branch position instead -- e.g.
`adult00_happy_s_01.png`, not `chokimaru_happy_01.png`. Several picture
files (one per frame) pack into one runtime lookup: `adult00_happy_s_01.png`
is a *file*, but what `hakoniwaos/src/creature.cpp`'s
`kf_creature_sprite_name()` actually builds, and what the code looks up in
the `.kfpack`, is `adult00_happy_s` -- no frame number, because that lookup
means "this pose's whole animation," not one picture of it. That function
is the authority on this scheme, not this manifest -- see
`tools/kf_character_manifest.py`'s own module docstring ("THE NAMING
CONVENTION") for the full breakdown of every token, including why
`_grudge_` sits where it does even though no runtime lookup asks for it
yet, and `SpriteSpec.filename` vs. `.entry_name` for where the file and
lookup names part company.

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

**"NO DESIGN BRIEF YET" is a warning the tool can still print, but does not
today.** It used to fire for the "egg" and "baby" stages, before either was
described anywhere -- the care-loop spec's addendum since gave both a design
(see "Egg and baby have designs now" there), and every entry in
`tools/character_manifest.toml` now has `has_design_brief = true`. The check
and the message stay in `tools/kf_prompt_builder.py` for the next stage or
entity that gets added without a design decided first -- it's a real
safeguard, just not currently tripped by anything in this roster.

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
directory held 49 source PNGs (48x48, RGBA) as of the third pass documented
below (2026-08-09); more passes have landed since without a matching write-up
here. **Currently 238 source PNGs** — all four teen forms (`teen0`-`teen3`)
are shipped now, and the ten adult forms are the only stage still missing
(`find examples/creature_demo/sprites -name '*.png' | wc -l` to recheck).
`assets.kfpack` is the packed result.

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

**Third pass (2026-08-09):** added the shrine (death scene), Marumaru
(the `child` life stage), and the first teen form (`hamaru`/`teen0`) --
31 sprites, bringing `examples/creature_demo/` to 49. Scoped deliberately:
the owner was hitting placeholder rectangles as soon as the demo pet grew
up, so this batch unblocked that rather than finishing the full roster at
the time (the remaining three teens and all ten adults were still unmade).
**Superseded since:** further passes (not individually written up here)
brought the roster to 238 source PNGs and shipped all four teen forms;
only the ten adult forms remain unmade as of this writing.

- **The shrine is a new *kind* of manifest entry, not a new creature life
  stage.** `simulator/src/pet/kf_creature_screen.cpp` looks it up by the
  literal name `shrine_idle_s`, never through
  `kf_creature_sprite_name()` -- a shrine is scenery, not a creature with a
  pose to look up. It needed exactly one sprite (one state, one direction),
  which no existing entity did, so `tools/kf_character_manifest.py` grew a
  `directions` per-entity override (defaulting to the usual three) and
  `iter_sprites()` picked up a fourth shared-stage table, `[stages.shrine]`,
  alongside egg/baby/child. See that file's own comments on both changes,
  and the manifest entry's comment for why the mechanism-reuse (same
  `[stages.X]` shape) doesn't make it an actual life stage.
- **Generated with `create_8_direction_object` even though only one
  direction shipped** -- for the shrine, not `create_1_direction_object`,
  specifically to keep the same camera angle (`view = "low top-down"`) as
  every creature sprite, so scenery and creature read as the same world.
  Only the south rotation was downloaded; the other seven were generated
  and discarded, which is the accepted cost of matching the camera exactly
  rather than guessing at an equivalent angle from a cheaper tool.
- **Marumaru (child) and Hamaru (teen0) both used the same
  `create_8_direction_object` (neutral base) + `create_object_state` (the
  other four care-loop states) sequence the second pass established for
  the baby** -- see that section above. No new technique was needed for
  either.
- **All three requests used `size=48` directly**, unlike the baby's 68px
  canvas in the second pass. `create_8_direction_object` returned exact
  48x48 PNGs for all three entities this time, so the crop/downscale
  post-process the second pass needed for the egg and baby was not
  necessary here -- confirmed by decoding each PNG's own IHDR before
  ingesting, not assumed.
- **Every generation call requested `size=48` and `view="low top-down"`**,
  matching the baby's own defaults, so the new sprites sit in the same
  camera/scale family without a second style negotiation.
- Ingest was run with `--id shrine`, `--id marumaru`, and `--id hamaru`
  first (all three: 100% ok, 0 missing, 0 invalid) before the unfiltered
  pack build, which -- with the roster still far short of the full set
  **at that point in the third pass** -- always reports "missing" for the
  ungenerated teens/adults and
  exits non-zero; that exit code does not mean the pack write failed. It
  still writes and independently re-verifies whatever *did* validate
  (`--strict` is the flag that would refuse to write on any gap, and
  wasn't used here on purpose). Check the "wrote ... / pack verification:
  OK" lines, not the process exit code, to see whether the write itself
  succeeded.

## If something's confusing

Every one of these commands accepts `--help` and will explain its own
options. If a name in the list looks wrong, don't fix it yourself --
**every character name in this project is an unverified placeholder**
(see `character_manifest.toml`'s own top comment) until it's been checked
for trademark conflicts, which hasn't happened yet.
