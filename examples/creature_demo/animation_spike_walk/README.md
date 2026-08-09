# Animation spike: child walk cycle

**This is a spike, not production art.** Do not pack these frames into
`assets.kfpack`, reference them from the manifest, or ship them. They exist
to answer one question: is PixelLab's animation quality good enough, and
what does it cost, before committing to animating the full creature roster.

## What this is

One PixelLab object animated, one animation, three directions:

- Creature: the child form (`marumaru`), object id
  `37f7629b-87fb-4ec6-871d-18ef0b28bce1` in the PixelLab account
  (description begins "A kawaii creature: a smooth, f..." — matches the
  shipped `child_*` sprites; confirmed by downloading its `south` rotation
  and byte-comparing to `examples/creature_demo/sprites/child_neutral_s_01.png`
  — see below).
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

See `.superpowers/sdd/animation-spike-report.md` at the repo root for the
full assessment (identity, readability, style, direction consistency,
measured generation cost, and byte cost at both raw RGB565 and 8-bit
indexed color).
