/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The creature as a thing you can see, as opposed to kf/pet.h's creature as a
 * set of numbers. Nothing here ever changes the simulation; every function
 * takes the pet as const and reads it.
 *
 * This lives in hakoniwaos/ rather than simulator/ because the device draws
 * the same creature the desktop does, from the same code. A presentation
 * layer that existed only on the desktop would be the emulator this project
 * does not have.
 */

#ifndef KF_CREATURE_H
#define KF_CREATURE_H

#include "kf/pet.h"
#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which sprite to show. These are the five states the character manifest
 * draws for every creature, plus dead, which has no art yet -- see
 * kf_creature_sprite_name() in Task 2 for what happens meanwhile. */
typedef enum {
    KF_CREATURE_POSE_NEUTRAL = 0,
    KF_CREATURE_POSE_HAPPY,
    KF_CREATURE_POSE_OBJECTING,
    KF_CREATURE_POSE_SICK,
    KF_CREATURE_POSE_SLEEPING,
    KF_CREATURE_POSE_DEAD,
    KF_CREATURE_POSE_COUNT
} kf_creature_pose;

/* Pick the pose for the pet as it stands right now.
 *
 * reaction_hold_ms is how long the most recent care reaction should still be
 * showing on the creature's body, counted down by the caller. It exists
 * because kf_pet_state::last_reaction is sticky -- it keeps the last reaction
 * forever -- so reading it directly would leave a creature grinning for the
 * rest of its life after one liked feed. Zero means the reaction has finished
 * being expressed.
 *
 * Precedence, strongest first: dead, sick, then the held reaction, then
 * neutral. Sleeping is never returned yet: nothing in Core can say the
 * creature is asleep (see the care-loop spec's "Sleep, settled" addendum);
 * the pose exists so the sprite table and the art are ready when it lands. */
kf_creature_pose kf_creature_pose_for(const kf_pet_state *pet,
                                      uint32_t reaction_hold_ms);

/* Write the asset-pack name for this stage and pose into out, always
 * NUL-terminated. Names follow tools/character_manifest.toml's convention,
 * <entity>_<state>_<frame>, because that manifest is what produced the art
 * and its filenames are the only contract between the two.
 *
 * The egg has exactly one state ("idle") because it has nothing to react to,
 * so every pose collapses to egg_idle_01 there.
 *
 * KF_CREATURE_POSE_DEAD has no art in the manifest yet -- the death scene is
 * unbuilt (care-loop spec section 7). It falls back to the sick sprite, which
 * is wrong-looking but visible, rather than to nothing at all, which would
 * look like a rendering bug. */
void kf_creature_sprite_name(kf_pet_stage stage, kf_creature_pose pose,
                             char *out, size_t out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_CREATURE_H */
