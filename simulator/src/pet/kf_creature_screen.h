/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Home, redrawn as a creature you can watch rather than a bars-and-buttons
 * LVGL screen (Task 4 of the pet-screen plan; see kf_screen_nav.cpp for the
 * routing that puts this here). Since Task 4 of the Lua game-layer plan
 * (docs/superpowers/plans/2026-08-12-lua-game-layer.md), this file does not
 * draw at all: it DECLARES the creature, the mess, the shrine and the stats
 * band to kf/scene.h's retained scene -- kf_scene_set_pos(), kf_scene_set_
 * sprite(), kf_scene_set_size() and friends -- once per frame, and
 * kf_scene_commit() (hakoniwaos/src/scene.cpp) is what diffs that against
 * what was last actually painted and turns the difference into the minimal
 * set of kf_blit_frame()/kf_blit_frame_mirrored()/kf_fill_rect() calls. The
 * sprite kf_creature_sprite_name() (kf/creature.h) resolves for the pet's
 * current pose and the wander's current facing, at whatever frame kf_
 * creature::anim's cursor is on, is a DECLARATION this file hands the
 * scene, not a draw call this file makes itself.
 *
 * kf_creature_pose_for()/kf_creature_update() are pure and take the pet as
 * const (kf/creature.h) -- nothing in this file, or reachable from it, ever
 * mutates the pet's simulation state. It only reads kf_pet_session_state()
 * and drives its OWN presentation-only kf_creature (position, wander
 * target, dwell, facing, held reaction), which is not saved and not part of
 * Core, and is not scene state either -- see kf_creature_screen.cpp's own
 * header comment for why it stays a plain struct outside kf/scene.h's
 * object table.
 */

#ifndef KF_CREATURE_SCREEN_H
#define KF_CREATURE_SCREEN_H

#include "kf/creature.h"

#include <cstdint>

/* Brings the screen up: creates the presentation-only kf_creature (kf/
 * creature.h) in the middle of its field and gives it a first idea of
 * where to walk, then paints the field's background for the very first
 * time. Call once, after kf_pet_session_init() -- kf_creature_screen_
 * frame() reads kf_pet_session_state() from its first call, the same
 * "real values from frame one" convention kf_pet_screen_init() already
 * uses for the LVGL Home screen this replaces. */
void kf_creature_screen_init(void);

/* Advance the creature's wander by dt_ms and re-declare it to the scene: its
 * sprite (or, if the asset pack has no art for this pet's stage/pose/
 * direction yet, whatever kf/scene.h falls back to for an unresolved
 * sprite name -- see kf/scene.h's own "WHAT HAPPENS WHEN THE SCENE IS
 * FULL"-adjacent comment on kf_scene_add_sprite()), at its new position, at
 * whatever frame its animation cursor is on (kf_creature::anim, ticked
 * separately -- see kf_creature_tick_anim()'s own comment, kf/creature.h).
 * Declaring the SAME position/sprite/frame as last frame costs nothing:
 * kf_scene_commit() (called once, at the end of this function) is what
 * notices nothing actually changed and skips repainting. At most 2 dirty
 * rectangles on a frame the creature genuinely moves -- see headless_
 * main.cpp's run_creature_screen_check() for what pins this budget down.
 * Call once per frame while this screen is the active one.
 *
 * Two exceptions, both by construction: while pet->stage == KF_PET_STAGE_EGG
 * the creature does not wander, only bobs gently in place (still declared
 * every frame, so it still costs at most the same 2 rects while the bob
 * offset is actually moving); once pet->dead, this declares a static
 * shrine instead and stops declaring the creature at all -- see kf_
 * creature_screen.cpp's own comment on both, and on why there is no longer
 * a separate "just died" vs "already dead" vs "just revived" case to get
 * wrong: the scene's own diff is what used to need three. */
void kf_creature_screen_frame(uint32_t dt_ms);

/* Re-declares every object this screen owns from scratch (kf_scene_reset(),
 * which invalidates every kf_scene_id a previous call to this function
 * handed out -- see kf/scene.h's "HANDLES, NOT POINTERS" comment) and
 * repaints the whole 240x320 panel, unconditionally -- WITHOUT touching the
 * creature's own simulation state (position, wander target, dwell, held
 * reaction), which keeps going exactly where it left off. kf_creature_
 * screen_init() already does this once at boot, as the very last thing it
 * does after creating the creature; kf_screen_nav.cpp calls this again
 * every later time Home becomes the active screen, from its own
 * screen-switch path, NOT from kf_creature_screen_frame() above.
 *
 * This exists because this screen does not own the whole framebuffer the
 * way an LVGL screen with its own dirty-tracking does: whatever screen was
 * active before it (Info, today) leaves real pixels behind when it stops
 * being active, and this screen's own per-frame declarations only ever
 * describe ITS OWN objects -- there is no way for the differ to know
 * anything else on the panel needs erasing too unless told. Skipping this
 * on entry means Home's own objects would repaint correctly but never
 * touch whatever Info last drew outside them, forever -- the same failure
 * shape as the black-trail bug ADR 0017 already hit once, for a different
 * reason. See docs/architecture/adr-0017-pet-screen.md:143-188. kf_scene_
 * reset()'s forced full-panel repaint (kf/scene.h's own comment on that
 * function) is what makes this a one-call fix rather than this file having
 * to enumerate every row Info might have touched. */
void kf_creature_screen_enter(void);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY below this line, the same status kf_screen_nav.h's own
 * "DEBUG/TEST ONLY" section has: not part of the gameplay surface, not
 * called by the interactive build. The wander picks its own facing from
 * the RNG (kf_creature_update(), hakoniwaos/src/creature.cpp), which a
 * test cannot steer without waiting on chance -- this lets a headless
 * check force a specific facing instead, so it can exercise resolve_
 * sprite()'s per-direction lookup (including the "_w_"-not-found ->
 * mirrored "_e_" fallback) deterministically. See headless_main.cpp's
 * run_creature_screen_sprite_check() for the actual caller.
 * --------------------------------------------------------------------- */

/* Same effect as a real care-button edge (Task 6's KF_BTN_A/UP/DOWN/LEFT/
 * RIGHT -- see kf_creature_screen.cpp's handle_care_buttons()), just
 * callable without one -- the same reasoning kf_screen_nav.h's own
 * kf_screen_nav_debug_advance()/_home() give for existing at all, rather
 * than routing a headless check through headless_input.cpp's single shared,
 * frame-indexed button script (KF_BTN_A/RIGHT/etc. already drive that
 * script's OWN cases for the sprite-bounce demo; adding care-button
 * windows to it would change what those unrelated, already-locked golden
 * checksums see too). `buttons` is a kf_button bitmask, applied once,
 * immediately -- unlike kf_creature_screen_debug_set_direction() below,
 * this does not wait for the next kf_creature_screen_frame() call, because
 * the real per-frame path does not either: kf_creature_screen_frame() acts
 * on kf_app_buttons_pressed() the moment it reads it, so this mirrors that
 * exactly rather than inventing a second timing model. */
void kf_creature_screen_debug_press(uint32_t buttons);

/* How long the current care reaction should still be showing on the
 * creature's body -- kf_creature::reaction_hold_ms, unexported otherwise
 * (kf_creature.h). Lets a headless check confirm a button-triggered care
 * action starts the SAME reaction-hold countdown a Lua-triggered one
 * already did (both go through the one `pet->care_actions_taken changed`
 * check in kf_creature_screen_frame()) without duplicating that check's
 * own logic in the test. */
uint32_t kf_creature_screen_debug_reaction_hold_ms(void);

/* The egg bob's current vertical offset in whole pixels (see
 * egg_bob_offset_y() in kf_creature_screen.cpp) -- lets a headless check
 * confirm the wobble is actually moving, and staying within its intended
 * amplitude, without duplicating the wave's own integer math or reaching
 * into this file's private elapsed-time counter. Meaningful only while
 * pet->stage == KF_PET_STAGE_EGG; kf_creature_screen_frame() only ever
 * applies this offset to what it draws under that same condition, so a
 * caller reading it for any other stage is reading a number nothing on
 * screen currently uses. */
int16_t kf_creature_screen_debug_egg_bob_offset_y(void);

/* Overrides the presentation-only creature's current facing. Takes effect
 * on the very next kf_creature_screen_frame() call and holds until changed
 * again or overwritten by real movement (kf_creature_update() only writes
 * ::dir on a frame where the creature actually moves -- pass dt_ms == 0 to
 * kf_creature_screen_frame() to guarantee it does not, the same trick
 * run_creature_screen_sprite_check() uses to hold a forced facing across
 * a resolve/draw call). Does not otherwise touch position, wander target,
 * dwell, or anything else about the creature. */
void kf_creature_screen_debug_set_direction(kf_creature_direction dir);

/* The creature's own wander position -- exactly kf_creature_bounds() on the
 * private kf_creature this file owns, exposed so a test can sample the
 * right rectangle of the framebuffer without duplicating this file's field
 * geometry or guessing at a bounding box from pixel content alone (content-
 * derived bounds are only as tight as whatever the sprite's own transparent
 * margins happen to be, which is not something a test should have to
 * assume is symmetric). This is what the creature's SCENE object is
 * positioned at each frame -- for every stage EXCEPT the egg, where the
 * scene object is declared at a small per-frame vertical offset from this
 * rect instead (the bob, see egg_bob_offset_y() and kf_creature_screen_
 * debug_egg_bob_offset_y() in kf_creature_screen.cpp), not this rect
 * exactly. This accessor deliberately still reports the UNshifted position
 * even then: it exists to prove the WANDER itself (whether the egg gate is
 * holding, whether a later stage's target-seeking is correct), and the bob
 * is a declare-time-only offset the wander state never sees or is
 * affected by. */
kf_rect kf_creature_screen_debug_bounds(void);

/* The creature's own animation cursor -- exactly g_creature.anim.frame
 * (kf/creature.h's kf_anim, ticked by kf_creature_tick_anim() and wrapped
 * against the resolved sprite's frame_count every frame this screen
 * declares the creature to the scene, see kf_creature_screen.cpp's
 * declare_creature()). Lets a headless check confirm the cursor is really
 * advancing/wrapping as this screen drives it, without duplicating kf_
 * anim's accumulator arithmetic in the test. */
uint16_t kf_creature_screen_debug_anim_frame(void);

/* Task 9 (docs/superpowers/plans/2026-08-11-hardware-bringup.md): the stats
 * band's three need bars, hunger/happiness/energy in that order -- index 0,
 * 1, 2 respectively, kf_pet_state's own field order (kf/pet.h) -- which this
 * header exposes ONLY as much as the corresponding creature/egg/shrine
 * accessors above already expose about THEIR own drawing, and for the same
 * reason: so a headless check can sample the right pixels or compare the
 * right number without reaching into kf_creature_screen.cpp's private
 * layout constants (kStatsRowsY0, kStatsBarW, etc) or duplicating their
 * arithmetic.
 *
 * The bounding rect of bar `index`'s full 0..100% track -- NOT however much
 * of it happens to be filled right now, which depends on the pet's live
 * need and is exactly what a test is trying to sample, not something to
 * bake into the rect it asks for. Each bar is two scene objects, a fixed
 * "track" box and a "fill" box on top of it (kf_creature_screen.cpp's own
 * stats-band comment); this reads the TRACK's bounds via kf_scene_bounds(),
 * which never changes after kf_creature_screen_enter() declares it.
 * `index` outside [0,3) returns an empty rect rather than reading past an
 * internal array -- a test passing a bad index should get an
 * obviously-wrong empty rect it can assert against, not undefined
 * behaviour. */
kf_rect kf_creature_screen_debug_stat_bar_bounds(int index);

/* How many of bar `index`'s own pixels are CURRENTLY declared as "filled"
 * -- the FILL box's own kf_scene_bounds() width, exactly the quantised
 * value declare_stat_bars() (kf_creature_screen.cpp) most recently set via
 * kf_scene_set_size(). Lets a test confirm a bar reflects the pet's
 * current need (quantised) without duplicating the quantisation arithmetic
 * or reverse-engineering it from raw pixels. `index` outside [0,3) returns
 * -1, the one case this accessor cannot express as a real width -- a fully
 * empty bar (need 0) legitimately reads back 0, not -1. */
int16_t kf_creature_screen_debug_stat_bar_filled_px(int index);

#endif /* KF_CREATURE_SCREEN_H */
