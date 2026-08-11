/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Task 5 of the Lua game-layer plan (docs/superpowers/plans/2026-08-12-lua-
 * game-layer.md): the single source of truth for "where the demo creature
 * is and what it looks like right now", extracted out of kf_creature_
 * screen.cpp so BOTH the C++ screen and the Lua screen can read the exact
 * same wander/pose/animation result on any given frame.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT IN LUA YET. The plan is explicit
 * (docs/superpowers/plans/2026-08-12-lua-game-layer.md, "What moves and
 * what stays"): the wander (kf_creature_update, choose_target) and pose/
 * sprite-name selection move to Lua in TASK 6, not this one -- moving them
 * while the renderer is also new would make an A/B diff impossible to
 * attribute. So for Task 5, the wander keeps running exactly once per
 * frame, in C++, and BOTH screen implementations call kf_creature_
 * presenter_advance() and then just read the result -- position, resolved
 * sprite name (including the west-mirror fallback), whether to draw
 * mirrored, and the animation frame. Only ONE implementation is active in
 * any given process at a time (KF_HOME_SCREEN chooses which, or the
 * headless parity check drives each half explicitly, resetting between
 * them), so there is exactly one advance() call per real frame, exactly as
 * there was before this file existed.
 *
 * WHAT DID NOT MOVE HERE. kf_creature_pose_for()/kf_creature_sprite_name()/
 * kf_creature_update() themselves (hakoniwaos/include/kf/creature.h) stay
 * exactly where they were -- this file only owns the ORCHESTRATION that
 * used to be inline in kf_creature_screen.cpp: which update path to call
 * (egg bob vs wander), when to reset the animation cursor, and the west-
 * mirror sprite-name fallback. */

#ifndef KF_CREATURE_PRESENTER_H
#define KF_CREATURE_PRESENTER_H

#include "kf/creature.h"
#include "kf/pet.h"
#include "kf/types.h"

#include <cstdint>

/* The play field both screen implementations wander the creature inside.
 * Owning this ONE constant here, rather than each screen implementation
 * defining its own copy, is what guarantees the two can never quietly
 * disagree about where the creature is allowed to walk -- see this file's
 * own header comment on why that guarantee is the whole point. The bottom
 * 60 rows (y=[260,320)) are the reserved stats band, kept out of the field
 * the same way kf_creature_screen.cpp's own kField always has been. */
constexpr kf_rect KF_CREATURE_PRESENTER_FIELD = {0, 0, 240, 260};

/* Brings the presenter's creature up in the middle of KF_CREATURE_PRESENTER_
 * FIELD with a first wander target already chosen -- exactly what kf_
 * creature_screen_init() used to do directly. Call ONCE, at process start,
 * not on every Home re-entry: the creature's wander state is not part of
 * "what this screen last painted" and keeps going exactly where it left off
 * across a Home -> Info -> Home cycle, the same guarantee kf_creature_
 * screen_enter()'s own header comment already documents for the pre-Task-5
 * code. */
void kf_creature_presenter_reset(void);

/* Advances the creature by dt_ms and resolves everything a screen needs to
 * declare it this frame. Must be called at most once per logical frame,
 * before reading any accessor below -- exactly the shape kf_creature_
 * screen_frame() used to have inline. `pet` must be non-null and outlive
 * the call; only read, never mutated (kf_creature_pose_for()/kf_creature_
 * update() are pure, and this function stays that way about the pet too).
 *
 * Handles, in order: noticing a new care action (starts the reaction hold),
 * the egg-vs-alive branch (bob in place vs real wander -- see kf_creature_
 * screen.cpp's egg-gate comment history for why the egg never wanders),
 * pose selection, the west-mirror sprite-name fallback (resolve the "_w_"
 * name first, fall back to the "_e_" name mirrored only if the pack has no
 * west art of its own), and resetting the animation cursor to frame 0
 * exactly on the frame the resolved name changes. */
void kf_creature_presenter_advance(const kf_pet_state *pet, uint32_t dt_ms);

/* Forces the NEXT kf_creature_presenter_advance() call to treat the
 * resolved sprite name as freshly changed, resetting the animation cursor
 * to frame 0 -- call on every Home re-entry, not just at process start, so
 * a returning player sees each pose's animation start clean rather than
 * continuing wherever it was mid-cycle when Home was last visible. Purely
 * a "what to compare the next resolved name against" reset: does not touch
 * position, wander target, dwell, or facing. Matches kf_creature_screen.cpp
 * 's pre-Task-5 behaviour of resetting its own g_last_requested_sprite_name
 * on every kf_creature_screen_enter() call. */
void kf_creature_presenter_force_anim_restart(void);

/* Where to draw the creature this frame, top-left corner, already including
 * the egg's bob offset when the pet is an egg. */
int16_t kf_creature_presenter_x(void);
int16_t kf_creature_presenter_y(void);

/* The resolved pack entry name -- the "_w_" name when the pack has real west
 * art, otherwise the "_e_" name (see kf_creature_presenter_mirrored() for
 * when to flip it). Never empty after the first kf_creature_presenter_
 * advance() call; the pointer is owned by this module and is valid until
 * the next advance() call, never freed -- safe to hand straight to a Lua
 * lua_pushstring() or a kf_scene_set_sprite() call without copying. */
const char *kf_creature_presenter_sprite_name(void);

/* True when the sprite above should be drawn mirrored (the "_e_" sprite
 * standing in for a missing "_w_" one) -- see kf/creature.h's kf_creature_
 * direction comment for the full west-facing story. */
bool kf_creature_presenter_mirrored(void);

/* The animation cursor for the sprite above, already wrapped to its real
 * frame count (kf_creature_anim_wrap()). */
uint16_t kf_creature_presenter_anim_frame(void);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY below this line -- same status as kf_creature_screen.h's
 * own "DEBUG/TEST ONLY" section, which these back directly now. See that
 * header for what each one is FOR; this file only supplies the
 * implementation the extraction moved.
 * --------------------------------------------------------------------- */

void kf_creature_presenter_debug_set_direction(kf_creature_direction dir);
uint32_t kf_creature_presenter_debug_reaction_hold_ms(void);
int16_t kf_creature_presenter_debug_egg_bob_offset_y(void);

/* The creature's own wander position, UNshifted by the egg's bob -- see
 * kf_creature_screen.h's kf_creature_screen_debug_bounds() for why a test
 * wants this rect specifically and not kf_creature_presenter_x()/_y()
 * above. */
kf_rect kf_creature_presenter_debug_bounds(void);

#endif /* KF_CREATURE_PRESENTER_H */
