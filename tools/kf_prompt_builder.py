#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Turn a character_manifest.toml sprite entry into a text prompt for an
image generator, and print it to stdout.

This file knows nothing about Pixellab, or any other generator -- it only
builds text. The thing that will eventually call an image generator with
this text is tools/kf_generate_sprites.py's generation seam, deliberately
kept separate so this half of the pipeline works today, with no connector.

The style block (art direction that is the SAME for every sprite in the
roster) and the per-state body-language guidance are kept apart from the
per-creature facts (object/read/quirk), matching the brief: "The
per-creature parts ... vary; the style block is constant."

WHAT'S SYNTHESIZED HERE, NOT QUOTED FROM THE BIBLE: the bible defines the
five care-loop states (happy/neutral/objecting/sick/sleeping) only by name
-- docs/superpowers/specs/2026-08-09-core-care-loop-design.md, not the
bible itself, is even where four of those five names come from. Neither
document says what body pose each state should be. STATE_GUIDANCE below is
this tool's own minimal, generic reading of "expression is carried by the
body" applied to each state name -- functional scaffolding to make the
pipeline runnable, not lore. Treat it as a first draft a human should
sanity-check per creature, not as bible content.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from kf_character_manifest import (
    ALL_STAGE_KEYS,
    DEFAULT_MANIFEST_PATH,
    SpriteSpec,
    iter_sprites,
    load_manifest,
)

# Bible section 3 ("The house style" + "The frozen-frame rule" + "Sprite
# size"), condensed into instructions rather than quoted as prose. Applied
# to every sprite, unconditionally.
#
# DELIBERATE DEVIATION FROM THE BIBLE (Chris, 2026-08-09): the bible's
# section 3 describes the medium as felt-tip marker on paper -- hand-wobble
# in the line, the look of a drawing. That reads as an instruction to
# imitate a hand-drawn medium, which an image generator answers with soft,
# wobbly, anti-aliased edges. At 48x48 that is the wrong output regardless
# of how good the drawing is. The medium below is stated as pixel art
# instead, in the terms a sprite generator actually responds to.
#
# Only the MEDIUM changed. Every rule under it -- two dots and a mouth, one
# exaggerated feature, one residue detail, the frozen-frame rule, the
# half-size silhouette test -- is the bible's and is unchanged, because
# those are design rules and hold in any medium.
#
# SECOND DEVIATION, additive this time (Chris, second art pass, 2026-08-09):
# the first pass of generated art read as thin and Western-illustration
# rather than the intended Japanese kawaii look. This is a standing house-
# style decision, not a one-batch instruction -- it applies to every
# creature generated from here on, so it lives here rather than in one-off
# prompt text. The KAWAII_SHAPE_BLOCK below is appended to every sprite's
# style block for that reason.
STYLE_BLOCK = """House style -- every sprite, no exceptions:
- A Tamagotchi-style creature sprite, drawn as pixel art. Chunky and readable at a glance, more mascot than monster.
- Flat blocks of solid color with clean, hard pixel edges and a dark outline. A small palette. No gradients, no anti-aliasing, no soft or painterly rendering, no sketched or hand-drawn line.
- The face is two dots and a mouth, nothing more. Expression comes from the BODY POSE, never the face.
- Exactly one feature exaggerated absurdly (a huge lip, a single tooth, a giant nose -- whatever this creature's read calls for).
- Deliberate asymmetry: lopsided eyes, one arm, something sticking out of the top. Avoid mirror symmetry.
- No consistent scale with other creatures in the roster -- draw this one at whatever size reads best on its own.
- Exactly ONE residue detail carried over from the source object (a seam, a coil, a scorch ring, a worn patch, a missing tooth). Never a costume, never the whole object stuck onto the body.
- The quirk must be visible in this single still frame. If a viewer can't tell what's wrong or funny about it from a motionless silhouette, it isn't done.
- Silhouette must stay legible shrunk to HALF the drawn size -- this is a small screen.
- Background: fully transparent (alpha channel), not a solid fill of any color. The asset pipeline keys the background out itself; do not draw one."""

# Japanese kawaii shape language, standing house style as of the second art
# pass (Chris, 2026-08-09): "cute, fat, blobby pixel sprites ... chubby,
# rounded, blobby", explicitly reacting against the first pass reading as
# thin and Western-illustration. Kept as its own block (appended after
# STYLE_BLOCK, not merged into it) so the two art-direction decisions --
# "this is pixel art, not a marker drawing" and "this is kawaii, not
# Western" -- stay separately attributable and separately editable.
KAWAII_SHAPE_BLOCK = """Kawaii shape language -- every sprite, no exceptions:
- Chubby and blobby, never thin or spindly. The body reads as one soft, rounded mass first and a creature second -- squash it wider and rounder than feels natural.
- No sharp angles, no straight edges longer than a pixel or two, no waistline or narrow joints. Every outline is a smooth, bulging curve.
- The silhouette should look like it would bounce or jiggle if you flicked it -- pixel-art plump, closer to a rice-cake or a plush toy than an illustrated sprite."""

# Bible section 6: shared grammar for the four juveniles, read from the
# manifest itself (meta.juvenile_shared_grammar) rather than duplicated
# here, so there is exactly one copy of this text in the repo.

STATE_GUIDANCE = {
    "idle": "No care-loop expression applies yet at this stage -- keep the pose completely neutral and at rest.",
    "happy": "Body language open and lifted: leaning or bouncing forward, the exaggerated feature raised or puffed up.",
    "neutral": "Default resting pose. This is the baseline every other state should read as a clear departure from.",
    "objecting": "Body turned partially away or braced, the exaggerated feature pulled back or drooping, weight shifted away from the viewer.",
    "sick": "Body slumped low, posture sagging, the exaggerated feature drooping or flattened. Convey illness through line and pose only -- no added iconography like sweat drops.",
    "sleeping": "Eyes shut (still two dots, drawn closed), body settled low and still, a breathing-at-rest posture.",
}

# The three views a sprite gets asked for: "s" (front), "e" (side), "n"
# (back) -- SpriteSpec.direction, sourced from tools/kf_character_manifest.py's
# DIRECTIONS, which in turn mirrors kf_creature_sprite_name()'s
# direction_token() (hakoniwaos/src/creature.cpp) exactly. No "w" entry here,
# deliberately: west is the "e" sprite mirrored at draw time (kf/blit.h's
# kf_blit_mirrored(), driven by simulator/src/pet/kf_creature_screen.cpp's
# west-first fallback lookup), not a fourth view a generator is ever asked
# to draw -- a real left-facing sprite is a decision for whoever draws that
# specific creature, not something this pipeline requests by default. See
# kf_character_manifest.py's "THE NAMING CONVENTION" comment for the fuller
# version of this same reasoning.
DIRECTION_GUIDANCE = {
    "s": "Front view: the creature faces the viewer head-on, square to camera. Both eyes (or eye-marks) and the mouth are visible and centred.",
    "e": "Side profile view: the creature is turned fully sideways, facing right, drawn as a clean profile silhouette -- not a 3/4 turn. Only the near eye reads; the exaggerated feature is drawn from whichever angle actually shows it best in profile.",
    "n": "Back view: the creature faces away from the viewer, showing its back. No face is visible from this angle -- read the pose from posture and outline alone, and carry the exaggerated feature and residue detail wherever they still show from behind.",
}

_NO_BRIEF = (
    "*** NO DESIGN BRIEF YET ***\n"
    "The character bible does not describe this stage's appearance -- it "
    "predates the bible's three named stages (see the care-loop spec's "
    "addendum). Do not generate art for this entry until a human decides "
    "what it looks like; this prompt is printed to show the pipeline "
    "plumbing works, not to be sent to a generator as-is."
)


def build_prompt(spec: SpriteSpec, meta: dict) -> str:
    lines = [f"# {spec.sprite_name}  ({spec.width}x{spec.height}px, frame "
             f"{spec.frame}/{spec.frame_count})"]

    if not spec.has_design_brief:
        lines.append(_NO_BRIEF)
        lines.append("")
        lines.append(STYLE_BLOCK)
        lines.append("")
        lines.append(KAWAII_SHAPE_BLOCK)
        return "\n".join(lines)

    name = spec.display_name or spec.entity_id
    verified_note = "" if spec.name_verified else " [UNVERIFIED PLACEHOLDER NAME -- bible section 9]"
    lines.append(f"Creature: {name}{verified_note}")
    if spec.stage == "teen":
        lines.append(f"Stage: juvenile ({spec.family} family)")
        lines.append(f"Shared juvenile grammar: {meta['juvenile_shared_grammar']}")
    elif spec.stage == "adult":
        family = spec.family or "none (orphan branch)"
        lines.append(f"Stage: adult ({family} family)")
    else:
        lines.append(f"Stage: {spec.stage}")

    if spec.object_:
        lines.append(f"Object (the seed, not the shape -- see the derivation rule): {spec.object_}")
    if spec.read:
        lines.append(f"Read: {spec.read}")
    if spec.quirk:
        lines.append(f"Quirk (job + wear + the one behavior that works against it): {spec.quirk}")
    if spec.note:
        lines.append(f"Context note: {spec.note}")
    if spec.render_note:
        lines.append(f"Render note: {spec.render_note}")

    lines.append("")
    lines.append(f"State: {spec.state}")
    lines.append(STATE_GUIDANCE.get(spec.state, "(no specific guidance for this state)"))

    lines.append("")
    lines.append(f"Direction: {spec.direction}")
    lines.append(DIRECTION_GUIDANCE.get(
        spec.direction, "(no specific guidance for this direction)"))

    if spec.variant == "grudge":
        lines.append("")
        lines.append("GRUDGE VARIANT:")
        lines.append(meta["grudge_grammar"])

    lines.append("")
    lines.append(STYLE_BLOCK)
    lines.append("")
    lines.append(KAWAII_SHAPE_BLOCK)
    return "\n".join(lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    p.add_argument("--id", help="only this entity id, e.g. chokimaru")
    p.add_argument("--stage", choices=list(ALL_STAGE_KEYS))
    p.add_argument("--state", help="only this state, e.g. happy")
    p.add_argument("--sprite", help="only this exact sprite name, e.g. baby_happy_e_01")
    p.add_argument("--skip-no-brief", action="store_true",
                    help="skip entries the bible doesn't describe (egg/baby) instead of printing the flag")
    args = p.parse_args(argv)

    raw = load_manifest(args.manifest)
    meta = raw["meta"]

    count = 0
    for spec in iter_sprites(raw):
        if args.id and spec.entity_id != args.id:
            continue
        if args.stage and spec.stage != args.stage:
            continue
        if args.state and spec.state != args.state:
            continue
        if args.sprite and spec.sprite_name != args.sprite:
            continue
        if args.skip_no_brief and not spec.has_design_brief:
            continue
        print(build_prompt(spec, meta))
        print("\n" + ("=" * 72) + "\n")
        count += 1

    if count == 0:
        print("no sprites matched those filters", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
