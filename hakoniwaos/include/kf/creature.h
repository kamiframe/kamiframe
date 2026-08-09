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

/* Which way the creature is facing, for sprite selection. Four facing
 * directions are served by three sprite sets: S is the front, toward the
 * viewer; E is the side; N is the back, away from the viewer. West is the
 * E sprite mirrored at draw time, so there is deliberately no W value here
 * -- see kf_creature_sprite_name(). */
typedef enum {
    KF_CREATURE_DIR_S = 0,
    KF_CREATURE_DIR_E,
    KF_CREATURE_DIR_N,
    KF_CREATURE_DIR_COUNT
} kf_creature_direction;

/* Write the asset-pack name for this pet's current stage/branch, this pose,
 * and this facing direction into out, always NUL-terminated (including on
 * truncation, and when out is null or out_len is 0, in which case out is
 * left untouched). Names follow tools/character_manifest.toml's convention,
 * <stage><indices>_<pose>_<dir>_<frame>, because that manifest is what
 * produced the art and its filenames are the only contract between the two.
 *
 * This takes the whole pet rather than just its stage because, from the teen
 * stage onward, the stage alone does not say which sprite: the roster
 * branches by kf_pet_state::teen_form and, for adults, also by
 * ::adult_branch. Egg/baby/child are shared single designs and ignore both
 * fields. Frame is always "01" -- multi-frame animation is unbuilt and out
 * of scope here.
 *
 * The egg has exactly one state ("idle") because it has nothing to react to,
 * so every pose collapses to egg_idle_<dir>_01 there.
 *
 * KF_CREATURE_POSE_DEAD has no art in the manifest yet -- the death scene is
 * unbuilt (care-loop spec section 7). It falls back to the sick sprite, which
 * is wrong-looking but visible, rather than to nothing at all, which would
 * look like a rendering bug. */
void kf_creature_sprite_name(const kf_pet_state *pet, kf_creature_pose pose,
                             kf_creature_direction dir, char *out,
                             size_t out_len);

/* Sub-pixel scale for creature positions. Movement is integer maths at 1/16th
 * of a pixel, the same approach hakoniwaos/src/demo.cpp uses, so a slow walk
 * is smooth without floating point in Core. */
#define KF_CREATURE_SUB 16

/* Presentation state for one creature. Not saved -- where the creature
 * happens to be standing is not worth persisting, and a fresh position on
 * load is indistinguishable from a remembered one. */
typedef struct {
    int32_t x;              /* 1/16th pixels, top-left of the sprite */
    int32_t y;
    int32_t target_x;       /* where it has decided to walk to */
    int32_t target_y;
    int16_t facing;         /* -1 facing left, +1 facing right */
    uint32_t dwell_ms;      /* how long it still intends to stand still */
    uint32_t reaction_hold_ms;
    uint32_t seen_care_actions; /* to notice when another care action lands */
} kf_creature;

/* Place the creature in the middle of the field and give it a first idea. */
void kf_creature_init(kf_creature *c, kf_rect field);

/* Advance one frame's worth of walking about. Pure apart from kf/rng.h, so a
 * seed reproduces a walk exactly. */
void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms);

/* Where the sprite currently sits, in whole pixels -- what to blit and what
 * to mark dirty. */
kf_rect kf_creature_bounds(const kf_creature *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_CREATURE_H */
