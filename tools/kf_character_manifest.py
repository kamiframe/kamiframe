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
names, and eventually Lua lookups) IS THE CODE'S, NOT THIS FILE'S OWN. The
pack entry name is the contract with hakoniwaos/src/creature.cpp's
kf_creature_sprite_name() -- that function is the authority on what a
runtime lookup asks for, and it does not change to suit this manifest; this
manifest changes to agree with it. See that function (and stage_token(),
pose_name(), direction_token() beside it) for the ground truth this module
is deriving from. What follows restates it in this file's own terms, not a
second, independent definition of it:

    <stage_token><branch_indices>_<pose>_<dir>[_grudge]_<frame>

  - stage_token+branch_indices: "egg", "baby", "child", "teen<N>" (N = which
    of the 4 verb families, 0-based), or "adult<N><M>" (N = the same teen
    family index, M = which adult within that family, 0-based) -- see
    CODE_TEEN_FORM_COUNT/CODE_ADULTS_PER_TEEN_FORM below for where N and M
    actually come from. Deliberately generic: no real creature name ever
    appears in a sprite_name, because every name in this manifest is an
    unverified trademark placeholder (docs/sdk-style-guide.md's "no
    creature names in code" rule). The real id ("chokimaru", "hamaru", ...)
    and display_name stay on SpriteSpec as PROMPT metadata only --
    tools/kf_prompt_builder.py genuinely needs to know which creature it is
    drawing even though the filename must not say so -- and never leak into
    sprite_name.
  - pose: one of the entity's states (by default happy/neutral/objecting/
    sick/sleeping), except the egg, which has exactly one state ("idle" in
    this manifest) and whose stage_token kf_creature_sprite_name() special-
    cases to ignore the pose argument entirely and always emit "idle" --
    see that function's own comment.
  - dir: "s" (front, KF_CREATURE_DIR_S), "e" (side, KF_CREATURE_DIR_E), or
    "n" (back, KF_CREATURE_DIR_N) -- every entity-pose gets all three. This
    manifest never generates "w": west is the "e" sprite mirrored at draw
    time by default (kf/blit.h's kf_blit_mirrored(), driven by
    simulator/src/pet/kf_creature_screen.cpp's west-first fallback lookup)
    -- a "_w_" name exists only for a pack that ships real hand-drawn
    left-facing art, which is a decision for whoever draws that specific
    sprite, not something a manifest generator predicts in advance.
  - grudge: present only for the grudge-variant half of an entity with
    grudge = true. Never present otherwise -- there is no "not_grudge"
    marker, absence IS the normal form. NOT part of kf_creature_sprite_name()
    today -- Core has no notion of a grudge pose yet (ADR 0015's care-
    mistake tracking is still an unbuilt, deferred design surface) -- so a
    grudge sprite_name is reserved art no runtime lookup can reach yet, kept
    in the same shape as everything else so it is ready the day that lookup
    exists, and named as an extra trailing token specifically so it can
    never collide with (or be mistaken for) a name the code actually asks
    for.
  - frame: always a 2-digit, 1-based frame index (01, 02, ...), even when
    an entity only has one frame -- matching kf_creature_sprite_name()'s own
    hardcoded "_01" for the (today, universal) single-frame case. Keeping
    the shape constant regardless of frame count means a filename glob or a
    Lua lookup never has to know in advance whether a given state is
    animated.

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

# The three directions kf_creature_sprite_name() (hakoniwaos/src/creature.cpp)
# will actually ask a pack for by default -- "s"/"e"/"n", matching
# direction_token()'s KF_CREATURE_DIR_S/E/N cases in that same file. "w" is
# deliberately excluded here -- see THE NAMING CONVENTION above for why.
# Order matters only for iter_sprites()'s stable output order, not for
# correctness.
DIRECTIONS = ("s", "e", "n")

# Mirrors KF_PET_TEEN_FORM_COUNT (hakoniwaos/include/kf/pet.h) and
# kAdultsInFamily (hakoniwaos/src/pet.cpp) exactly -- imported by value, not
# by import, for the same cross-language reason PACK_NAME_BYTES above is
# (that header/source pair has no reason to export Python-facing constants
# of its own). CODE_ADULTS_PER_TEEN_FORM is indexed by teen_form, in the
# SAME order kAdultsInFamily is: 0=cut, 1=hold, 2=mark, 3=go -- which this
# file derives from character_manifest.toml's own [[teens]] order (see
# iter_sprites()'s teen_form_index below), not from this tuple; this tuple
# exists only so validate_manifest() can catch the two ever drifting apart,
# rather than the manifest silently producing adult<N><M> tokens Core's own
# kf_pet_adults_in_family() would treat as out of range.
CODE_TEEN_FORM_COUNT = 4
CODE_ADULTS_PER_TEEN_FORM = (2, 3, 3, 1)

# The stage/branch token kf_creature_sprite_name() builds for an adult with
# no verb family at all (Hokorimaru, the "dust" branch reached by never
# interacting with a creature -- bible section 7-8): teen_form is
# KF_PET_TEEN_FORM_DUST, defined in kf/pet.h as exactly equal to
# KF_PET_TEEN_FORM_COUNT so it sits one past the four real families, and
# kf_pet_adults_in_family() returns 1 for that out-of-range input -- so
# branch 0 is the only valid slot a family-less adult can occupy. Exactly
# one entity in this manifest has no family, matching that one slot;
# validate_manifest() below is what would catch it if a second family-less
# adult were ever added without Core also growing a second dust
# adult_branch to receive it.
DUST_FORM_INDEX = CODE_TEEN_FORM_COUNT
DUST_BRANCH_INDEX = 0

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
    entity_id: str            # "chokimaru", "hamaru", "marumaru", "egg", ... --
                               # PROMPT metadata only, never part of sprite_name
                               # (see THE NAMING CONVENTION above)
    display_name: str | None
    stage: str                # "egg" | "baby" | "child" | "teen" | "adult"
    state: str                # "happy", "neutral", ... or "idle" for the egg
    direction: str            # "s" | "e" | "n" -- see DIRECTIONS above
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
    # code_token == stage_key ("egg"/"baby"/"child") for every shared stage:
    # kf_creature_sprite_name()'s stage_token() emits exactly that literal
    # for KF_PET_STAGE_EGG/BABY/CHILD, no branch indices -- these three
    # stages are shared single designs, so there is nothing to index.
    yield from _sprites_for_entity(entity, entity_id, stage_key, entity["stage"],
                                    meta, grudge_eligible=False)


def _sprites_for_entity(entity: dict, entity_id: str, code_token: str, stage: str,
                         meta: dict, *, grudge_eligible: bool) -> Iterator[SpriteSpec]:
    """code_token is the generic stage/branch token kf_creature_sprite_name()
    would build for this entity ("egg", "baby", "child", "teen<N>",
    "adult<N><M>") -- what actually goes into sprite_name. entity_id is the
    real creature id, kept on the yielded SpriteSpec as prompt metadata only
    -- see THE NAMING CONVENTION at the top of this file."""
    states = _entity_states(entity, meta)
    frame_count = _entity_frames(entity, meta)
    width, height = _entity_size(entity, meta)
    grudge = grudge_eligible and entity.get("grudge", False)
    variants: list[str | None] = [None, GRUDGE_VARIANT] if grudge else [None]

    for state in states:
        for variant in variants:
            for direction in DIRECTIONS:
                for frame in range(1, frame_count + 1):
                    parts = [code_token, state, direction]
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
                        direction=direction,
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
    teens = raw.get("teens", [])
    families = raw.get("families", [])

    for stage_key in ("egg", "baby", "child"):
        yield from _iter_shared_stage(stage_key, raw, meta)

    # teen_form_index: a teen's position in [[teens]] IS its teen_form --
    # kf/pet.h's own comment on teen_form ("a plain 0-based index
    # identifying WHICH branch of the evolution tree was taken") gives no
    # other source of truth for what number a given teen is, so this
    # manifest's ordering has to BE that source of truth. Also used below to
    # resolve each family (and, through it, each adult) back to the same
    # index.
    teen_form_index: dict[str, int] = {}
    for teen in teens:
        _validate_id("teen", teen["id"])
        teen_form_index[teen["id"]] = len(teen_form_index)
        yield from _sprites_for_entity(
            teen, teen["id"], f"teen{teen_form_index[teen['id']]}", "teen",
            meta, grudge_eligible=False)

    # family_form_index: which teen_form an adult's family descends from,
    # via that family's teen_id -- e.g. family "cut" -> teen_id "hamaru" ->
    # teen_form_index["hamaru"]. Resolved through teen_form_index rather
    # than a family's position in [[families]] because it is the TEEN that
    # owns a teen_form number (kf/pet.h again), and the family/teen link is
    # exactly what validate_manifest() already cross-checks below.
    family_form_index: dict[str, int] = {
        fam["id"]: teen_form_index[fam["teen_id"]]
        for fam in families
        if fam.get("teen_id") in teen_form_index
    }

    for adult in raw.get("adults", []):
        _validate_id("adult", adult["id"])
        fam_id = adult.get("family") or None
        if fam_id:
            if fam_id not in family_form_index:
                raise ManifestError(
                    f"adult '{adult['id']}': family '{fam_id}' has no "
                    "resolvable teen_form -- its [[families]] entry is "
                    "missing or its teen_id doesn't match a [[teens]] id")
            form_index = family_form_index[fam_id]
            fam = next(f for f in families if f["id"] == fam_id)
            try:
                branch_index = fam["adults"].index(adult["id"])
            except ValueError:
                raise ManifestError(
                    f"adult '{adult['id']}': family '{fam_id}' does not "
                    "list it in its own adults = [...] -- a code_token "
                    "can't be derived without that link") from None
        else:
            # No family: the "dust" branch (Hokorimaru) -- see
            # DUST_FORM_INDEX/DUST_BRANCH_INDEX above.
            form_index = DUST_FORM_INDEX
            branch_index = DUST_BRANCH_INDEX
        code_token = f"adult{form_index}{branch_index}"
        yield from _sprites_for_entity(adult, adult["id"], code_token, "adult",
                                        meta, grudge_eligible=True)


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

    # Cross-check against Core's own constants (see CODE_TEEN_FORM_COUNT's
    # comment above): if these ever drift apart, iter_sprites() is silently
    # building adult<N><M> tokens kf_pet_adults_in_family() would treat as
    # out of range, which is exactly the kind of mismatch this whole task
    # exists to eliminate -- so it is checked here mechanically rather than
    # left to a future reader to notice by inspection.
    teens = raw.get("teens", [])
    if len(teens) != CODE_TEEN_FORM_COUNT:
        problems.append(
            f"{len(teens)} [[teens]] entries, but CODE_TEEN_FORM_COUNT "
            f"(mirroring KF_PET_TEEN_FORM_COUNT in hakoniwaos/include/kf/"
            f"pet.h) says {CODE_TEEN_FORM_COUNT} -- update whichever one is "
            "stale")
    else:
        for teen_form, teen in enumerate(teens):
            fam = families.get(teen.get("family"))
            expected = CODE_ADULTS_PER_TEEN_FORM[teen_form]
            actual = len(fam.get("adults", [])) if fam else 0
            if actual != expected:
                problems.append(
                    f"teen '{teen['id']}' (teen_form {teen_form}): "
                    f"{actual} adult(s) in its family, but "
                    f"CODE_ADULTS_PER_TEEN_FORM (mirroring kAdultsInFamily "
                    f"in hakoniwaos/src/pet.cpp) says {expected} -- update "
                    "whichever one is stale")

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
        if args.direction and spec.direction != args.direction:
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
    listp.add_argument("--direction", choices=list(DIRECTIONS), help="filter to one facing direction")

    args = p.parse_args(argv)
    raw = load_manifest(args.manifest)

    if args.command == "stats":
        return _cmd_stats(raw, args)
    if args.command == "list":
        return _cmd_list(raw, args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
