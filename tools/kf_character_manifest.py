#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
"""Load tools/character_manifest.toml and turn it into the flat list of
sprites the roster actually needs, stdlib only.

This module has no opinions about art (see kf_prompt_builder.py) or files
on disk (see kf_generate_sprites.py / kf_ingest_sprites.py). Its only job is
"read the manifest, and give every other tool the same list of sprites and
the same names for them" -- one parser, shared, the same reasoning
tools/kf_pack_assets.py's own docstring gives for not inventing a second
pack format for audio.

THE NAMING CONVENTION (shared by the manifest, PNG filenames, .kfpack entry
names, and eventually Lua lookups):

    <entity_id>_<state>[_grudge]_<frame>

  - entity_id: "egg", "baby", "marumaru" (the shared child design), one of
    the four teen ids (hamaru, nigimaru, tenmaru, ayumaru), or one of the
    ten adult ids. Lowercase ASCII letters/digits only -- see
    _validate_id() -- so the joining "_" is never ambiguous.
  - state: one of the entity's states (by default happy/neutral/objecting/
    sick/sleeping; "idle" for the egg -- see character_manifest.toml).
  - grudge: present only for the grudge-variant half of an entity with
    grudge = true. Never present otherwise -- there is no "not_grudge"
    marker, absence IS the normal form.
  - frame: always a 2-digit, 1-based frame index (01, 02, ...), even when
    an entity only has one frame. Keeping the shape constant regardless of
    frame count means a filename glob or a Lua lookup never has to know in
    advance whether a given state is animated.

Every part of that name is validated against ASSET_TYPE_SPRITE's 32-byte
`name` field (tools/kf_pack_assets.py's own DIRECTORY_ENTRY_BYTES layout,
31 usable characters after the implicit terminator) by validate_manifest()
below -- see its docstring for what "usable" means here.
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

# Mirrors tools/kf_pack_assets.py's NAME_BYTES / DIRECTORY_ENTRY_BYTES
# exactly -- imported by value, not by import, because that module is a
# packer with no reason to export naming-length constants of its own. If
# that field's size ever changes, both files need the same edit; there is
# no way to enforce that mechanically beyond this comment (kf_pack_assets.py
# has the identical caveat about KF_FLASH_ASSET_BUDGET_BYTES and
# partitions.csv, for the same reason).
PACK_NAME_BYTES = 32
PACK_NAME_MAX_CHARS = PACK_NAME_BYTES - 1  # one byte reserved as terminator

DEFAULT_MANIFEST_PATH = Path(__file__).parent / "character_manifest.toml"

_ID_RE = re.compile(r"^[a-z][a-z0-9]*$")
_STATE_RE = re.compile(r"^[a-z][a-z0-9]*$")

GRUDGE_VARIANT = "grudge"


@dataclass(frozen=True)
class SpriteSpec:
    """One sprite that needs to exist. Every field a prompt builder, an
    ingest validator, or a future Lua lookup could need, denormalized onto
    the sprite itself so nothing downstream has to re-join against the
    manifest."""

    sprite_name: str          # the full naming-convention string, no extension
    entity_id: str            # "chokimaru", "hamaru", "marumaru", "egg", ...
    display_name: str | None
    stage: str                # "egg" | "baby" | "child" | "teen" | "adult"
    state: str                # "happy", "neutral", ... or "idle" for the egg
    frame: int                # 1-based
    frame_count: int          # this state's total frame count
    variant: str | None       # None (normal) or GRUDGE_VARIANT
    width: int
    height: int
    name_verified: bool
    has_design_brief: bool
    family: str | None
    object_: str | None
    read: str | None
    quirk: str | None
    note: str | None
    family_heading_note: str | None

    @property
    def filename(self) -> str:
        return f"{self.sprite_name}.png"


class ManifestError(ValueError):
    """A manifest that fails to load, or fails validation."""


def load_manifest(path: Path = DEFAULT_MANIFEST_PATH) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def _validate_id(kind: str, value: str) -> None:
    if not _ID_RE.match(value):
        raise ManifestError(
            f"{kind} id '{value}' must be lowercase ASCII letters/digits "
            "only (no underscores or hyphens) -- the naming convention "
            "joins fields with '_', so an id containing one would be "
            "ambiguous to split back apart")


def _entity_states(entity: dict, meta: dict) -> list[str]:
    states = entity.get("states", meta["states"])
    for s in states:
        if not _STATE_RE.match(s):
            raise ManifestError(f"state '{s}' must be lowercase ASCII letters/digits only")
    return states


def _entity_frames(entity: dict, meta: dict) -> int:
    frames = entity.get("frames", meta["default_frames"])
    if frames < 1:
        raise ManifestError(f"entity '{entity.get('id')}': frames must be >= 1, got {frames}")
    return frames


def _entity_size(entity: dict, meta: dict) -> tuple[int, int]:
    size = entity.get("size", meta["default_size"])
    return size, size


def _iter_shared_stage(stage_key: str, raw: dict, meta: dict) -> Iterator[SpriteSpec]:
    entity = raw["stages"][stage_key]
    entity_id = entity.get("id", stage_key)
    _validate_id("stage", entity_id)
    yield from _sprites_for_entity(entity, entity_id, entity["stage"], meta,
                                    grudge_eligible=False)


def _sprites_for_entity(entity: dict, entity_id: str, stage: str, meta: dict,
                         *, grudge_eligible: bool) -> Iterator[SpriteSpec]:
    states = _entity_states(entity, meta)
    frame_count = _entity_frames(entity, meta)
    width, height = _entity_size(entity, meta)
    grudge = grudge_eligible and entity.get("grudge", False)
    variants: list[str | None] = [None, GRUDGE_VARIANT] if grudge else [None]

    for state in states:
        for variant in variants:
            for frame in range(1, frame_count + 1):
                parts = [entity_id, state]
                if variant:
                    parts.append(variant)
                parts.append(f"{frame:02d}")
                sprite_name = "_".join(parts)
                yield SpriteSpec(
                    sprite_name=sprite_name,
                    entity_id=entity_id,
                    display_name=entity.get("display_name"),
                    stage=stage,
                    state=state,
                    frame=frame,
                    frame_count=frame_count,
                    variant=variant,
                    width=width,
                    height=height,
                    name_verified=entity.get("name_verified", False),
                    has_design_brief=entity.get("has_design_brief", False),
                    family=entity.get("family") or None,
                    object_=entity.get("object"),
                    read=entity.get("read"),
                    quirk=entity.get("quirk"),
                    note=entity.get("note"),
                    family_heading_note=entity.get("family_heading_note"),
                )


def iter_sprites(raw: dict) -> Iterator[SpriteSpec]:
    """Yields every SpriteSpec the manifest implies, in a stable order:
    egg, baby, child, teens (manifest order), adults (manifest order)."""
    meta = raw["meta"]

    for stage_key in ("egg", "baby", "child"):
        yield from _iter_shared_stage(stage_key, raw, meta)

    for teen in raw.get("teens", []):
        _validate_id("teen", teen["id"])
        yield from _sprites_for_entity(teen, teen["id"], "teen", meta,
                                        grudge_eligible=False)

    for adult in raw.get("adults", []):
        _validate_id("adult", adult["id"])
        yield from _sprites_for_entity(adult, adult["id"], "adult", meta,
                                        grudge_eligible=True)


def validate_manifest(raw: dict) -> list[str]:
    """Returns a list of problems (empty if none). Does not raise -- the
    CLI decides what to do with a non-empty list; kf_ingest_sprites.py
    calls this too, before trusting the manifest for validation."""
    problems: list[str] = []
    seen_names: set[str] = set()

    for spec in iter_sprites(raw):
        if len(spec.sprite_name) > PACK_NAME_MAX_CHARS:
            problems.append(
                f"sprite name '{spec.sprite_name}' is {len(spec.sprite_name)} "
                f"chars, over the {PACK_NAME_MAX_CHARS}-char limit a "
                f".kfpack entry name allows (32-byte field, ADR 0033)")
        if spec.sprite_name in seen_names:
            problems.append(f"duplicate sprite name '{spec.sprite_name}'")
        seen_names.add(spec.sprite_name)

    # Cross-check family membership: every family's teen_id/adults should
    # resolve, and every teen/adult with a family should be listed by it.
    families = {f["id"]: f for f in raw.get("families", [])}
    teens_by_family: dict[str, str] = {t["id"]: t.get("family") for t in raw.get("teens", [])}
    adults_by_family: dict[str, str | None] = {
        a["id"]: (a.get("family") or None) for a in raw.get("adults", [])
    }

    for fam_id, fam in families.items():
        teen_id = fam.get("teen_id")
        if teen_id and teens_by_family.get(teen_id) != fam_id:
            problems.append(
                f"family '{fam_id}': teen_id '{teen_id}' does not point "
                "back to this family")
        for adult_id in fam.get("adults", []):
            if adults_by_family.get(adult_id) != fam_id:
                problems.append(
                    f"family '{fam_id}': adult '{adult_id}' does not point "
                    "back to this family")

    for adult_id, fam_id in adults_by_family.items():
        if fam_id and fam_id not in families:
            problems.append(f"adult '{adult_id}': unknown family '{fam_id}'")

    return problems


# --------------------------------------------------------------------------
# CLI: `stats` (counts + name-length sanity) and `list` (filterable dump of
# sprite names, one per line -- pipeable into anything).
# --------------------------------------------------------------------------

def _cmd_stats(raw: dict, args) -> int:
    specs = list(iter_sprites(raw))
    problems = validate_manifest(raw)

    by_stage: dict[str, int] = {}
    for s in specs:
        by_stage[s.stage] = by_stage.get(s.stage, 0) + 1

    longest = max(specs, key=lambda s: len(s.sprite_name))

    print(f"manifest: {args.manifest}")
    print(f"total sprites: {len(specs)}")
    for stage in ("egg", "baby", "child", "teen", "adult"):
        print(f"  {stage:6s}: {by_stage.get(stage, 0)}")
    print(f"distinct entities: {len({s.entity_id for s in specs})}")
    print(f"longest sprite name: '{longest.sprite_name}' "
          f"({len(longest.sprite_name)} chars, limit {PACK_NAME_MAX_CHARS})")
    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("\nno problems found")
    return 0


def _cmd_list(raw: dict, args) -> int:
    for spec in iter_sprites(raw):
        if args.stage and spec.stage != args.stage:
            continue
        if args.id and spec.entity_id != args.id:
            continue
        if args.state and spec.state != args.state:
            continue
        print(spec.sprite_name)
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH,
                    help="path to character_manifest.toml")
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("stats", help="print sprite counts and validate names")

    listp = sub.add_parser("list", help="list sprite names, one per line")
    listp.add_argument("--stage", choices=["egg", "baby", "child", "teen", "adult"])
    listp.add_argument("--id", help="filter to one entity id, e.g. chokimaru")
    listp.add_argument("--state", help="filter to one state, e.g. happy")

    args = p.parse_args(argv)
    raw = load_manifest(args.manifest)

    if args.command == "stats":
        return _cmd_stats(raw, args)
    if args.command == "list":
        return _cmd_list(raw, args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
