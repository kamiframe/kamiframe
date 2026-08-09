/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf/creature.h"

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
