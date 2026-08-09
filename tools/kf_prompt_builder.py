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
    DEFAULT_MANIFEST_PATH,
    SpriteSpec,
    iter_sprites,
    load_manifest,
)

# Bible section 3 ("The house style" + "The frozen-frame rule" + "Sprite
# size"), condensed into instructions rather than quoted as prose. Applied
# to every sprite, unconditionally.
STYLE_BLOCK = """House style -- every sprite, no exceptions:
- Felt-tip marker on paper: flat fills, visible hand-wobble in the line. No gradients, no shading, no soft rendering.
- The face is two dots and a mouth, nothing more. Expression comes from the BODY POSE, never the face.
- Exactly one feature exaggerated absurdly (a huge lip, a single tooth, a giant nose -- whatever this creature's read calls for).
- Deliberate asymmetry: lopsided eyes, one arm, something sticking out of the top. Avoid mirror symmetry.
- No consistent scale with other creatures in the roster -- draw this one at whatever size reads best on its own.
- Exactly ONE residue detail carried over from the source object (a seam, a coil, a scorch ring, a worn patch, a missing tooth). Never a costume, never the whole object stuck onto the body.
- The quirk must be visible in this single still frame. If a viewer can't tell what's wrong or funny about it from a motionless silhouette, it isn't done.
- Silhouette must stay legible shrunk to HALF the drawn size -- this is a small screen.
- Background: fully transparent (alpha channel), not a solid fill of any color. The asset pipeline keys the background out itself; do not draw one."""

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

    lines.append("")
    lines.append(f"State: {spec.state}")
    lines.append(STATE_GUIDANCE.get(spec.state, "(no specific guidance for this state)"))

    if spec.variant == "grudge":
        lines.append("")
        lines.append("GRUDGE VARIANT:")
        lines.append(meta["grudge_grammar"])

    lines.append("")
    lines.append(STYLE_BLOCK)
    return "\n".join(lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    p.add_argument("--id", help="only this entity id, e.g. chokimaru")
    p.add_argument("--stage", choices=["egg", "baby", "child", "teen", "adult"])
    p.add_argument("--state", help="only this state, e.g. happy")
    p.add_argument("--sprite", help="only this exact sprite name, e.g. chokimaru_happy_01")
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
