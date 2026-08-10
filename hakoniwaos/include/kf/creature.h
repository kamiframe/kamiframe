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
 * directions, served by three sprite SETS: S is the front, toward the
 * viewer; E is the side; N is the back, away from the viewer. W is the
 * other side -- but it is not a fourth sprite set, and it is not always a
 * mirror either. Mirroring is a capability, not a rule: some creatures ship
 * real hand-drawn "_w_" art and some do not, and whoever draws the sprite
 * for a given facing is the one making that call, not this file. The
 * caller resolving a name for KF_CREATURE_DIR_W (simulator/src/pet/
 * kf_creature_screen.cpp today) asks the pack for the "_w_" sprite first,
 * and only draws the "_e_" sprite mirrored when the pack has no west art of
 * its own -- see kf_creature_sprite_name()'s own comment for the naming
 * half of that and kf/blit.h's kf_blit_mirrored() for the drawing half.
 * (An earlier version of this comment said W deliberately did not exist at
 * all; that was true before any west-facing art existed to ask for, and is
 * superseded now that it might.) */
typedef enum {
    KF_CREATURE_DIR_S = 0,
    KF_CREATURE_DIR_E,
    KF_CREATURE_DIR_N,
    KF_CREATURE_DIR_W,
    KF_CREATURE_DIR_COUNT
} kf_creature_direction;

/* Write the .kfpack ENTRY name for this pet's current stage/branch, this
 * pose, and this facing direction into out, always NUL-terminated
 * (including on truncation, and when out is null or out_len is 0, in which
 * case out is left untouched). Names follow tools/character_manifest.toml's
 * convention, <stage><indices>_<pose>_<dir>, because that manifest is what
 * produced the art and its filenames are the only contract between the two.
 *
 * This takes the whole pet rather than just its stage because, from the teen
 * stage onward, the stage alone does not say which sprite: the roster
 * branches by kf_pet_state::teen_form and, for adults, also by
 * ::adult_branch. Egg/baby/child are shared single designs and ignore both
 * fields.
 *
 * NO FRAME NUMBER. A pack entry holds EVERY frame of an animation
 * contiguously (see KF_ASSET_TYPE_SPRITE_INDEXED in kf/assets.h), so a name
 * ending "_01" would claim to be frame one of something it is in fact all
 * of. Which frame to draw is a runtime argument to kf_blit_frame()
 * (kf/blit.h), not part of the name -- which also means an animated pose
 * needs no naming change at all: the pose already picks the sprite, and an
 * animated one simply has more frames behind the same name. PNG FILES on
 * disk do keep a frame number, because one file really is one frame; see
 * tools/kf_character_manifest.py's SpriteSpec.filename versus its
 * .entry_name for where those two concepts part company.
 *
 * The egg has exactly one state ("idle") because it has nothing to react to,
 * so every pose collapses to egg_idle_<dir> there.
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

/* How long one animation frame is held, in milliseconds.
 *
 * 100ms is 10fps, in the middle of the 8-12fps band hand-drawn character
 * animation reads best at, and DELIBERATELY UNRELATED to the display's
 * KF_TARGET_FPS of 30: animation timing is an art decision and refresh rate
 * is a hardware one, and tying them together would mean a panel that ran at
 * 60 made every creature move twice as fast. The accumulator below is what
 * keeps the two independent. */
#define KF_ANIM_FRAME_MS 100u

/* An animation cursor. Integer milliseconds only -- hakoniwaos/ has no
 * floating point (kf/budget.h's own reasoning: the device's FPU is for Lua,
 * and Core stays exact and cheap).
 *
 * accum_ms CARRIES ITS REMAINDER across advances rather than resetting to
 * zero. At a 33ms display tick, three ticks is 99ms and four is 132ms; a
 * reset-to-zero cursor would take four ticks every time and run at 7.6fps
 * instead of 10, and a nine-frame cycle would come out 40% slow. Carrying
 * the remainder makes the AVERAGE rate exactly KF_ANIM_FRAME_MS regardless
 * of what dt_ms happens to be, which also means the same dt_ms sequence
 * produces the same frame sequence on every machine -- what the headless
 * checks' synthetic clock relies on.
 *
 * `frame` runs UNBOUNDED here -- kf_creature_tick_anim() below has no idea
 * how many frames the resolved sprite has, and is not being taught: that
 * would mean handing Core's wander a sprite pointer for no reason. Bringing
 * it back into range for a particular sprite is kf_creature_anim_wrap()'s
 * job, called by whoever just resolved that sprite (simulator/src/pet/
 * kf_creature_screen.cpp's resolve_sprite()/draw path today), not this
 * struct's own concern. */
typedef struct {
    uint32_t accum_ms;
    uint16_t frame;
} kf_anim;

/* Presentation state for one creature. Not saved -- where the creature
 * happens to be standing is not worth persisting, and a fresh position on
 * load is indistinguishable from a remembered one. */
typedef struct {
    int32_t x;              /* 1/16th pixels, top-left of the sprite */
    int32_t y;
    int32_t target_x;       /* where it has decided to walk to */
    int32_t target_y;
    kf_creature_direction dir; /* which way it's currently facing -- see
                                 * kf_creature_direction above. Updated only
                                 * on frames where the creature actually
                                 * moves (kf_creature_update()); it holds
                                 * its last value through a whole dwell
                                 * rather than snapping to some default the
                                 * instant it stops. */
    uint32_t dwell_ms;      /* how long it still intends to stand still */
    uint32_t reaction_hold_ms;
    uint32_t seen_care_actions; /* to notice when another care action lands */
    kf_anim anim;            /* the sprite's own playback cursor, see kf_anim
                               * above -- ticked by kf_creature_tick_anim(),
                               * NOT by kf_creature_update() alone (see that
                               * function's own comment for why the egg needs
                               * a second caller). */
} kf_creature;

/* Place the creature in the middle of the field and give it a first idea. */
void kf_creature_init(kf_creature *c, kf_rect field);

/* Advance one frame's worth of walking about. Pure apart from kf/rng.h, so a
 * seed reproduces a walk exactly. Calls kf_creature_tick_anim() first thing,
 * before the dwell early return, so a dwelling (or sleeping, once that
 * exists) creature still animates in place. */
void kf_creature_update(kf_creature *c, kf_rect field, uint32_t dt_ms);

/* Advance the creature's animation cursor by one frame's worth of wall time.
 *
 * Called first thing by kf_creature_update(), AND directly by a caller that
 * deliberately skips the wander -- today that is the egg (simulator/src/pet/
 * kf_creature_screen.cpp), which sits still by design and would otherwise be
 * the one thing in the game that never animates, precisely backwards from
 * what was asked for. One implementation, two callers, rather than a second
 * copy of the same arithmetic -- the same shape handle_care_buttons() in
 * that file already uses for exactly this reason. A null c or a dt_ms of 0
 * is a no-op. */
void kf_creature_tick_anim(kf_creature *c, uint32_t dt_ms);

/* Bring the cursor back in range for an animation of `frame_count` frames,
 * resetting to 0 when it is past the end (or when frame_count is 0). Called
 * when the resolved sprite CHANGES: a 9-frame walk leaves the cursor at 7,
 * and a 3-frame objecting pose has no frame 7 -- kf_blit_frame() (kf/blit.h)
 * would clamp that to frame 0 anyway, so nothing reads out of bounds, but
 * the animation would visibly jump. Resets rather than wraps, so a pose
 * change starts at the beginning of its own cycle instead of somewhere
 * arbitrary inherited from the pose before it. A null c is a no-op. */
void kf_creature_anim_wrap(kf_creature *c, uint16_t frame_count);

/* Where the sprite currently sits, in whole pixels -- what to blit and what
 * to mark dirty. */
kf_rect kf_creature_bounds(const kf_creature *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_CREATURE_H */
