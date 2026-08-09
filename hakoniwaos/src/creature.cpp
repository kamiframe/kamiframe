/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf/creature.h"

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
