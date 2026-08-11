/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf/creature.h"
#include "kf/rng.h"

#include <stdio.h>

kf_creature_pose kf_creature_pose_for(const kf_pet_state *pet,
                                      uint32_t reaction_hold_ms) {
    if (pet == nullptr) {
        return KF_CREATURE_POSE_NEUTRAL;
    }
    if (pet->dead) {
        return KF_CREATURE_POSE_DEAD;
    }
    if (pet->sick) {
        return KF_CREATURE_POSE_SICK;
    }
    if (pet->asleep) {
        return KF_CREATURE_POSE_SLEEPING;
    }
    if (reaction_hold_ms > 0u) {
        if (pet->last_reaction == KF_PET_REACTION_LIKED) {
            return KF_CREATURE_POSE_HAPPY;
        }
        if (pet->last_reaction == KF_PET_REACTION_DISLIKED) {
            return KF_CREATURE_POSE_OBJECTING;
        }
    }
    return KF_CREATURE_POSE_NEUTRAL;
}

namespace {

/* Writes the <stage><indices> token -- "egg", "baby", "child", "teen<N>",
 * or "adult<teen_form><adult_branch>" -- into out. Nothing downstream of
 * this file ever sees a real creature name: tools/character_manifest.toml's
 * teen and adult sprites are individually named per branch, but every name
 * in that manifest is an unverified trademark placeholder (see
 * docs/sdk-style-guide.md), so this writes the plain branch indices instead
 * -- the same "opaque index, not a name" contract kf/pet.h's own
 * teen_form/adult_branch fields already keep.
 *
 * A null pet or an out-of-range stage falls back to "baby", the same
 * defensive default kf/pet.h's own out-of-range handling uses elsewhere. */
void stage_token(const kf_pet_state *pet, char *out, size_t out_len) {
    const kf_pet_stage stage = (pet != nullptr) ? pet->stage : KF_PET_STAGE_BABY;
    switch (stage) {
    case KF_PET_STAGE_EGG:
        snprintf(out, out_len, "egg");
        return;
    case KF_PET_STAGE_CHILD:
        snprintf(out, out_len, "child");
        return;
    case KF_PET_STAGE_TEEN:
        /* pet is never null here: a null pet always takes the BABY case
         * above, so this branch is only reached with a real pet. */
        snprintf(out, out_len, "teen%u", (unsigned)pet->teen_form);
        return;
    case KF_PET_STAGE_ADULT:
        snprintf(out, out_len, "adult%u%u", (unsigned)pet->teen_form,
                 (unsigned)pet->adult_branch);
        return;
    case KF_PET_STAGE_BABY:
    default:
        snprintf(out, out_len, "baby");
        return;
    }
}

const char *pose_name(kf_creature_pose pose) {
    switch (pose) {
    case KF_CREATURE_POSE_HAPPY: return "happy";
    case KF_CREATURE_POSE_OBJECTING: return "objecting";
    case KF_CREATURE_POSE_SICK: return "sick";
    case KF_CREATURE_POSE_SLEEPING: return "sleeping";
    case KF_CREATURE_POSE_DEAD: return "sick"; /* no death art yet */
    case KF_CREATURE_POSE_NEUTRAL:
    default: return "neutral";
    }
}

/* "s"/"e"/"n"/"w" -- see kf_creature_direction in kf/creature.h for what
 * each one means, including why "w" is a real token here even though the
 * manifest may or may not have real art behind it for a given creature (the
 * caller's job, not this function's -- see kf_creature_direction's own
 * comment). An out-of-range value falls back to "s", the front-facing
 * sprite, rather than reading past the enum. */
const char *direction_token(kf_creature_direction dir) {
    switch (dir) {
    case KF_CREATURE_DIR_E: return "e";
    case KF_CREATURE_DIR_N: return "n";
    case KF_CREATURE_DIR_W: return "w";
    case KF_CREATURE_DIR_S:
    default: return "s";
    }
}

} // namespace

void kf_creature_sprite_name(const kf_pet_state *pet, kf_creature_pose pose,
                             kf_creature_direction dir, char *out,
                             size_t out_len) {
    if (out == nullptr || out_len == 0u) {
        return;
    }
    const kf_pet_stage stage = (pet != nullptr) ? pet->stage : KF_PET_STAGE_BABY;
    const char *dir_tok = direction_token(dir);
    if (stage == KF_PET_STAGE_EGG) {
        /* The manifest gives the egg exactly one state, "idle" -- see
         * character_manifest.toml's [stages.egg] -- so every pose collapses
         * here regardless of what was asked for. Direction still varies:
         * the egg is a single design, not a single sprite.
         *
         * This includes KF_CREATURE_POSE_SLEEPING: eggs do not sleep
         * (kf_pet_state::asleep never becomes true for an egg -- see
         * apply_stage_segment()'s early return for KF_PET_STAGE_EGG in
         * hakoniwaos/src/pet.cpp, and ADR 0048), so kf_creature_pose_for()
         * should never actually hand this function SLEEPING for an egg-
         * stage pet in practice. This collapse is what would happen even
         * if it somehow did -- there is no egg_sleeping art in the shipped
         * pack, and there deliberately never needs to be one. */
        snprintf(out, out_len, "egg_idle_%s", dir_tok);
        return;
    }
    /* Task 7 (docs/superpowers/plans/2026-08-13-screens-clock-sleep.md,
     * ADR 0049): ADULT has no `adult*_sleeping_*` art in the shipped pack
     * at all (the 18 sleeping sprites cover baby/child/teen0-3 only), so
     * an adult that falls asleep resolves a name this pack does not
     * contain. Deliberately NOT special-cased the way EGG is above: unlike
     * the egg (which never asks for SLEEPING in the first place), an adult
     * genuinely can be asleep, and the honest behaviour is the SAME
     * missing-sprite fallback every other absent name in this pack already
     * gets (kf/scene.h's placeholder box, logged once) -- not a silent
     * substitution for a pose that has no real art. Generating real adult
     * sleeping art, or picking a specific stand-in pose, is Chris's call. */
    char stage_buf[16];
    stage_token(pet, stage_buf, sizeof(stage_buf));
    snprintf(out, out_len, "%s_%s_%s", stage_buf, pose_name(pose), dir_tok);
}

namespace {

/* Whole pixels per second, slow enough to read as an animal pottering about
 * rather than a cursor being dragged. */
constexpr int32_t kSpeedPxPerSec = 18;
constexpr int32_t kSpriteSize = 48;
constexpr uint32_t kDwellMinMs = 400u;
constexpr uint32_t kDwellSpreadMs = 2600u;

/* How much room the creature's top-left corner has to move in one axis,
 * so that its far edge (top-left + kSpriteSize) never passes hi. A field
 * narrower than the sprite has no room at all -- clamped to 0 rather than
 * going negative, which would otherwise turn "no room" into "walk off the
 * left/top edge". */
int32_t field_span(int16_t lo, int16_t hi) {
    const int32_t span = (int32_t)hi - (int32_t)lo - kSpriteSize;
    return span > 0 ? span : 0;
}

/* Pick a new spot inside the field. kf_rng_below(span+1) is uniform over
 * [0, span], so the top-left corner lands anywhere from the field's lo edge
 * up to (hi - kSpriteSize) inclusive -- the far edge lands on hi at most,
 * never past it, which is what keeps kf_creature_bounds() inside the
 * half-open field rect (kf/types.h) even at the extreme case.
 *
 * Deliberately does not touch c->dir: which way the creature FACES is a
 * property of which way it is actually walking THIS frame, decided in
 * kf_creature_update() once movement toward whatever target this function
 * picked is under way -- not of the moment the target was chosen, which can
 * sit several dwell-frames before the first step ever taken toward it. */
void choose_target(kf_creature *c, kf_rect field) {
    const int32_t sx = field_span(field.x0, field.x1);
    const int32_t sy = field_span(field.y0, field.y1);
    c->target_x = ((int32_t)field.x0 + (int32_t)kf_rng_below((uint32_t)sx + 1u)) *
                  KF_CREATURE_SUB;
    c->target_y = ((int32_t)field.y0 + (int32_t)kf_rng_below((uint32_t)sy + 1u)) *
                  KF_CREATURE_SUB;
}

/* Which way (dx, dy) points, as a facing direction: the axis with the
 * larger magnitude wins, N/S for vertical and E/W for horizontal. A tie
 * (equal magnitude on both axes, including the degenerate 0,0 case) goes to
 * the horizontal axis -- arbitrary, but it has to pick one, and there is no
 * more principled reason to prefer N/S over E/W here than the reverse.
 * Shared by kf_creature_init() (the very first facing, before any frame of
 * movement has happened) and kf_creature_update()'s movement branch (every
 * facing after that, always computed from that frame's actual travel --
 * see kf_creature::dir's own comment in kf/creature.h). */
kf_creature_direction direction_for_delta(int32_t dx, int32_t dy) {
    const int32_t abs_dx = dx < 0 ? -dx : dx;
    const int32_t abs_dy = dy < 0 ? -dy : dy;
    if (abs_dy > abs_dx) {
        return (dy < 0) ? KF_CREATURE_DIR_N : KF_CREATURE_DIR_S;
    }
    return (dx < 0) ? KF_CREATURE_DIR_W : KF_CREATURE_DIR_E;
}

} // namespace

void kf_creature_init(kf_creature *c, kf_rect field) {
    if (c == nullptr) { return; }
    c->x = ((int32_t)field.x0 + field_span(field.x0, field.x1) / 2) *
           KF_CREATURE_SUB;
    c->y = ((int32_t)field.y0 + field_span(field.y0, field.y1) / 2) *
           KF_CREATURE_SUB;
    c->dwell_ms = kDwellMinMs;
    c->reaction_hold_ms = 0u;
    c->seen_care_actions = 0u;
    c->anim.accum_ms = 0u;
    c->anim.frame = 0u;
    choose_target(c, field);
    /* c->dir has to be a defined value from the moment this returns --
     * kDwellMinMs is never 0, so a fresh creature sits through its first
     * dwell before kf_creature_update()'s movement branch would otherwise
     * ever get a chance to set it, and that dwell is exactly the window a
     * boot screenshot is most likely to catch. Facing toward the target
     * already chosen above, via the same axis-dominance rule
     * kf_creature_update() uses every frame after this one, means that
     * window shows a genuinely correct facing rather than an arbitrary
     * placeholder waiting to be overwritten. */
    c->dir = direction_for_delta(c->target_x - c->x, c->target_y - c->y);
}

void kf_creature_tick_anim(kf_creature *c, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }
    c->anim.accum_ms += dt_ms;
    while (c->anim.accum_ms >= KF_ANIM_FRAME_MS) {
        c->anim.accum_ms -= KF_ANIM_FRAME_MS;
        c->anim.frame = static_cast<uint16_t>(c->anim.frame + 1u);
    }
}

void kf_creature_anim_wrap(kf_creature *c, uint16_t frame_count) {
    if (c == nullptr) { return; }
    if (frame_count == 0u || c->anim.frame >= frame_count) {
        c->anim.frame = 0u;
    }
}

void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }
    kf_creature_tick_anim(c, dt_ms);

    if (c->reaction_hold_ms > dt_ms) {
        c->reaction_hold_ms -= dt_ms;
    } else {
        c->reaction_hold_ms = 0u;
    }

    if (c->dwell_ms > 0u) {
        c->dwell_ms = (c->dwell_ms > dt_ms) ? (c->dwell_ms - dt_ms) : 0u;
        return;
    }

    const int32_t step =
        ((int32_t)dt_ms * kSpeedPxPerSec * KF_CREATURE_SUB) / 1000;
    if (step <= 0) { return; }

    const int32_t dx = c->target_x - c->x;
    const int32_t dy = c->target_y - c->y;

    /* Facing is a property of THIS frame's actual travel -- computed here,
     * from the distance still remaining before the movement below consumes
     * it, not from wherever the creature ends up. On arrival both deltas
     * are zero, and recomputing from a (0,0) delta would snap the sprite to
     * direction_for_delta()'s horizontal tie-break (east) every single time
     * the creature stops walking, which is wrong -- see kf_creature::dir's
     * own comment in kf/creature.h for why it has to hold its last value
     * through the dwell instead. Guarding on dx/dy being nonzero, rather
     * than on `arrived` (computed below), also covers the rare case where a
     * freshly chosen target lands exactly on the creature's current spot:
     * there is no real movement to face at all that frame, so dir is left
     * exactly as it was. */
    if (dx != 0 || dy != 0) {
        c->dir = direction_for_delta(dx, dy);
    }

    /* Never overshoot: whenever the remaining distance on an axis is within
     * one step, snap straight to the target instead of adding/subtracting
     * step and passing it. That is what keeps an arbitrarily large dt_ms
     * from ever landing outside the field -- the target itself is always in
     * bounds (choose_target()), and every intermediate position is either
     * strictly closer to it or exactly it, never past it. */
    bool arrived = true;
    if (dx > step)       { c->x += step; arrived = false; }
    else if (dx < -step) { c->x -= step; arrived = false; }
    else                 { c->x = c->target_x; }

    if (dy > step)       { c->y += step; arrived = false; }
    else if (dy < -step) { c->y -= step; arrived = false; }
    else                 { c->y = c->target_y; }

    if (arrived) {
        c->dwell_ms = kDwellMinMs + kf_rng_below(kDwellSpreadMs);
        choose_target(c, field);
    }
}

kf_rect kf_creature_bounds(const kf_creature *c) {
    kf_rect r = {0, 0, 0, 0};
    if (c == nullptr) { return r; }
    r.x0 = (int16_t)(c->x / KF_CREATURE_SUB);
    r.y0 = (int16_t)(c->y / KF_CREATURE_SUB);
    r.x1 = (int16_t)(r.x0 + kSpriteSize);
    r.y1 = (int16_t)(r.y0 + kSpriteSize);
    return r;
}
