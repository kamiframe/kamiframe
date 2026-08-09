/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf/creature.h"
#include "kf/rng.h"

#include <stdio.h>
#include <string.h>

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

/* tools/character_manifest.toml's stage entities are keyed "egg", "baby",
 * "child" for these first three code stages; teen and adult sprites are not
 * in the manifest yet (only the four juveniles and ten confirmed adults,
 * keyed by creature id, not by stage) -- "baby" is this file's own fallback
 * for anything out of range, same defensive default kf/pet.h's own
 * out-of-range handling uses elsewhere. */
const char *stage_name(kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG: return "egg";
    case KF_PET_STAGE_BABY: return "baby";
    case KF_PET_STAGE_CHILD: return "child";
    case KF_PET_STAGE_TEEN: return "teen";
    case KF_PET_STAGE_ADULT: return "adult";
    default: return "baby";
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

} // namespace

void kf_creature_sprite_name(kf_pet_stage stage, kf_creature_pose pose,
                             char *out, size_t out_len) {
    if (out == nullptr || out_len == 0u) {
        return;
    }
    if (stage == KF_PET_STAGE_EGG) {
        /* The manifest gives the egg exactly one state, "idle" -- see
         * character_manifest.toml's [stages.egg] -- so every pose collapses
         * here regardless of what was asked for. */
        snprintf(out, out_len, "egg_idle_01");
        return;
    }
    snprintf(out, out_len, "%s_%s_01", stage_name(stage), pose_name(pose));
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

/* Pick a new spot inside the field and face towards it. kf_rng_below(span+1)
 * is uniform over [0, span], so the top-left corner lands anywhere from the
 * field's lo edge up to (hi - kSpriteSize) inclusive -- the far edge lands
 * on hi at most, never past it, which is what keeps kf_creature_bounds()
 * inside the half-open field rect (kf/types.h) even at the extreme case. */
void choose_target(kf_creature *c, kf_rect field) {
    const int32_t sx = field_span(field.x0, field.x1);
    const int32_t sy = field_span(field.y0, field.y1);
    c->target_x = ((int32_t)field.x0 + (int32_t)kf_rng_below((uint32_t)sx + 1u)) *
                  KF_CREATURE_SUB;
    c->target_y = ((int32_t)field.y0 + (int32_t)kf_rng_below((uint32_t)sy + 1u)) *
                  KF_CREATURE_SUB;
    c->facing = (c->target_x < c->x) ? (int16_t)-1 : (int16_t)1;
}

} // namespace

void kf_creature_init(kf_creature *c, kf_rect field) {
    if (c == nullptr) { return; }
    c->x = ((int32_t)field.x0 + field_span(field.x0, field.x1) / 2) *
           KF_CREATURE_SUB;
    c->y = ((int32_t)field.y0 + field_span(field.y0, field.y1) / 2) *
           KF_CREATURE_SUB;
    c->facing = 1;
    c->dwell_ms = kDwellMinMs;
    c->reaction_hold_ms = 0u;
    c->seen_care_actions = 0u;
    choose_target(c, field);
}

void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms) {
    if (c == nullptr || dt_ms == 0u) { return; }

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

    /* Never overshoot: whenever the remaining distance on an axis is within
     * one step, snap straight to the target instead of adding/subtracting
     * step and passing it. That is what keeps an arbitrarily large dt_ms
     * from ever landing outside the field -- the target itself is always in
     * bounds (choose_target()), and every intermediate position is either
     * strictly closer to it or exactly it, never past it. */
    bool arrived = true;
    const int32_t dx = c->target_x - c->x;
    if (dx > step)       { c->x += step; arrived = false; }
    else if (dx < -step) { c->x -= step; arrived = false; }
    else                 { c->x = c->target_x; }

    const int32_t dy = c->target_y - c->y;
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
