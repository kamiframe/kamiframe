# Animation spike: child walk cycle

**This is a spike, not production art.** Do not pack these frames into
`assets.kfpack`, reference them from the manifest, or ship them. They exist
to answer one question: is PixelLab's animation quality good enough, and
what does it cost, before committing to animating the full creature roster.

## What this is

One PixelLab object animated, one animation, three directions:

- Creature: the child life stage's PixelLab object (description begins "A
  kawaii creature: a smooth, f..." — matches the shipped `child_*` sprites;
  confirmed by downloading its `south` rotation and byte-comparing to
  `examples/creature_demo/sprites/child_neutral_s_01.png` — see below).
  Deliberately not naming the PixelLab account object id here: it identifies
  a specific asset in a specific account, not something a reader cloning
  this repo can resolve or needs.
- Animation: "gentle walk cycle, calm easy stroll"
- Mode: `v3` (default, not `pro`)
- Directions: south, east, north (the three the game actually uses; west is
  mirrored from east at runtime, so it was not generated)
- Frame count: 8 generated frames per direction, plus the reference/idle
  frame kept as frame 0 (`keep_first_frame=true`, the v3 default) = 9 stored
  frames per direction, 27 frames total.

## Layout

```
south/frame_0.png .. frame_8.png
east/frame_0.png  .. frame_8.png
north/frame_0.png .. frame_8.png
```

## Frame 0 vs. the shipped still

`south/frame_0.png` is **byte-identical** to
`examples/creature_demo/sprites/child_neutral_s_01.png` (verified with
`cmp`). This animation used the object's existing idle frame as its start
pose, so the shipped stills would not need to be regenerated if the whole
roster were animated this way.

## Verdict

**Not good enough to build the pipeline around as-is.** Identity and house
style hold up across all 27 frames — same proportions, same palette, no
drift into a different creature or a different rendering style. But this
life stage has no visible legs in its base design (it's a seated/rounded
blob), and v3's "gentle walk cycle" for it amounts to a barely-shifting
silhouette plus most of the frame-to-frame difference landing in the face
rather than the body — frames 0 and 4 of the south direction are almost
indistinguishable. At 48x48 in-game size this reads as a gentle idle
bob/blink, not a walk. The three directions also don't move by consistent
amounts (east has slightly more motion than south/north), and one frame
(north, frame 2) has a small stray rendering artifact at the silhouette
edge.

Shipping this without further prompt iteration (e.g. describing a
bounce/hop instead of a walk, since these creatures don't have legs to
stride with) or a per-frame QA pass would be a visible downgrade risk, not
a clear win. If this is worth pursuing further, the next cheap experiment
is a differently-worded animation on the same object, not scaling the
current prompt to the whole roster.

Cost: 43 generations for one animation across 3 directions (~14.3/direction
at v3 default), well inside the monthly allowance. Byte cost: 27 frames at
48x48 is ~121.5 KiB raw RGB565 or ~60.75 KiB 8-bit indexed per animated
pose (dropping to ~108 KiB / ~54 KiB if frame 0 is de-duplicated against
the already-shipped still, since it is byte-identical) — roughly 0.5%
(raw) or 0.25% (indexed) of the 12 MiB flash asset budget for a single
animation on a single life stage. Not the blocking constraint for one
pose; becomes one at full roster scale.
