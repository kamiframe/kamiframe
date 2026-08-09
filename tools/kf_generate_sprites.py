#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Drive character_manifest.toml + kf_prompt_builder.py through an image
generator, writing one PNG per sprite into an output directory.

THIS FILE CONTAINS THE ONE GENERATION SEAM. Everything either side of
generate_sprite() works today, with no generator connected: the manifest
loads, the prompts build, and this CLI can tell you exactly which files it
would have asked for and where they'd land. Only the actual network/tool
call inside generate_sprite() is missing, and it fails loudly rather than
silently when called.

When the Pixellab MCP server is connected, generate_sprite() is the only
function that needs to change -- fill in the body with a real call, keep
the same signature (spec, prompt, out_path) -> None, raising on failure.
Nothing in main() below needs to know how the call is made.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from kf_character_manifest import DEFAULT_MANIFEST_PATH, SpriteSpec, iter_sprites, load_manifest
from kf_prompt_builder import build_prompt


# --------------------------------------------------------------------------
# THE GENERATION SEAM
# --------------------------------------------------------------------------
def generate_sprite(spec: SpriteSpec, prompt: str, out_path: Path) -> None:
    """Ask an image generator for one sprite and write it to out_path as a
    PNG matching spec.width x spec.height, with transparency where the
    background should be keyed out.

    NOT IMPLEMENTED: the Pixellab MCP server this is meant to call is not
    connected to this environment yet. Everything upstream of this
    function (the manifest, the prompt builder, the naming convention) and
    everything downstream (kf_ingest_sprites.py) works without it -- this
    is deliberately the only place that knows a generator should exist.

    To make this real once the Pixellab MCP server is connected:
      1. Call it with `prompt` and (spec.width, spec.height) as the target
         size -- the prompt text already encodes everything about this
         sprite (creature, state, variant, style) that the generator needs.
      2. Write the returned image bytes to `out_path` as a PNG. It MUST
         have real alpha transparency where the background should be --
         kf_ingest_sprites.py validates that and will reject a flat opaque
         PNG.
      3. Raise on any failure (bad response, wrong size, generator error)
         instead of writing a partial or placeholder file -- nothing here
         should ever silently produce a wrong-sized or blank sprite.
      4. Leave this docstring's shape (spec, prompt, out_path) -> None
         alone; kf_ingest_sprites.py and this file's own main() do not need
         to change to pick up a real implementation.
    """
    raise RuntimeError(
        "the Pixellab MCP server is not connected yet.\n\n"
        "What this means: tools/kf_generate_sprites.py knows exactly which "
        f"sprite it wants ('{spec.sprite_name}', {spec.width}x{spec.height}px) "
        "and has already built a full text prompt for it (see "
        "tools/kf_prompt_builder.py), but has nothing to send that prompt "
        "to. No image was generated and no file was written.\n\n"
        "What to do about it: connect the Pixellab MCP server to this "
        "environment, then implement the body of generate_sprite() in "
        "tools/kf_generate_sprites.py -- that one function is the only "
        "thing this file is waiting on; see its own docstring for the "
        "exact contract it needs to satisfy.\n\n"
        "In the meantime: art can still be produced by hand or by any "
        "other tool and dropped into a directory using this manifest's "
        "naming convention (python3 tools/kf_character_manifest.py list), "
        "then run through tools/kf_ingest_sprites.py, which does not "
        "depend on this function at all.")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    p.add_argument("-o", "--out-dir", type=Path, required=True,
                    help="directory to write generated PNGs into")
    p.add_argument("--id", help="only this entity id, e.g. chokimaru")
    p.add_argument("--stage", choices=["egg", "baby", "child", "teen", "adult"])
    p.add_argument("--dry-run", action="store_true",
                    help="list what would be generated and exit, without "
                         "calling the (currently missing) generator")
    args = p.parse_args(argv)

    raw = load_manifest(args.manifest)
    meta = raw["meta"]

    targets = []
    for spec in iter_sprites(raw):
        if args.id and spec.entity_id != args.id:
            continue
        if args.stage and spec.stage != args.stage:
            continue
        if not spec.has_design_brief:
            continue
        targets.append(spec)

    if not targets:
        print("no sprites matched those filters (or all matches lack a "
              "design brief -- see kf_prompt_builder.py)", file=sys.stderr)
        return 1

    print(f"{len(targets)} sprite(s) targeted, writing into {args.out_dir}")
    if args.dry_run:
        for spec in targets:
            print(f"  would generate: {args.out_dir / spec.filename}")
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for spec in targets:
        prompt = build_prompt(spec, meta)
        out_path = args.out_dir / spec.filename
        try:
            generate_sprite(spec, prompt, out_path)
            print(f"  wrote {out_path}")
        except RuntimeError as e:
            print(f"  FAILED {spec.sprite_name}: {e}", file=sys.stderr)
            failures += 1
            break  # every call fails identically right now; no point repeating it

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
