/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Home, redrawn as a creature you can watch rather than a bars-and-buttons
 * LVGL screen (Task 4 of the pet-screen plan; see kf_screen_nav.cpp for the
 * routing that puts this here). Draws straight into the framebuffer through
 * kf/blit.h -- kf_fill_rect() to erase the creature's previous position,
 * kf_blit()/kf_blit_mirrored() to draw the sprite kf_creature_sprite_name()
 * (kf/creature.h) resolves for the pet's current pose and the wander's
 * current facing -- rather than through any LVGL widget tree. This is the
 * "core owns the pixels, a thin per-frame loop drives it" split kf/demo.cpp
 * already established for the placeholder bouncing sprite this replaces as
 * Home's owner; kf_creature.h itself lives in hakoniwaos/ for the same
 * reason (see that header's own comment).
 *
 * kf_creature_pose_for()/kf_creature_update() are pure and take the pet as
 * const (kf/creature.h) -- nothing in this file, or reachable from it, ever
 * mutates the pet's simulation state. It only reads kf_pet_session_state()
 * and drives its OWN presentation-only kf_creature (position, wander
 * target, dwell, facing, held reaction), which is not saved and not part of
 * Core.
 */

#ifndef KF_CREATURE_SCREEN_H
#define KF_CREATURE_SCREEN_H

#include <cstdint>

/* Brings the screen up: creates the presentation-only kf_creature (kf/
 * creature.h) in the middle of its field and gives it a first idea of
 * where to walk, then paints the field's background for the very first
 * time. Call once, after kf_pet_session_init() -- kf_creature_screen_
 * frame() reads kf_pet_session_state() from its first call, the same
 * "real values from frame one" convention kf_pet_screen_init() already
 * uses for the LVGL Home screen this replaces. */
void kf_creature_screen_init(void);

/* Advance the creature's wander by dt_ms and redraw it: erase its previous
 * 48x48 rectangle back to the field's background, then draw the sprite (or,
 * if the asset pack has no art for this pet's stage/pose/direction yet, a
 * placeholder rectangle -- see kf_creature_screen.cpp's own comment) at its
 * new position. Exactly two kf_fill_rect()/kf_blit() calls' worth of dirty
 * rectangles per frame, by construction -- see kf_screen_nav.h's own
 * per-frame contract for why only the active screen's update runs at all,
 * and headless_main.cpp's run_creature_screen_check() for what pins this
 * budget down. Call once per frame while this screen is the active one. */
void kf_creature_screen_frame(uint32_t dt_ms);

/* Repaints the whole field and forgets wherever the creature was last
 * drawn on screen -- WITHOUT touching the creature's own simulation state
 * (position, wander target, dwell, held reaction), which keeps going
 * exactly where it left off. kf_creature_screen_init() already does this
 * once at boot, as the very last thing it does after creating the
 * creature; kf_screen_nav.cpp calls this again every later time Home
 * becomes the active screen, from its own screen-switch path, NOT from
 * kf_creature_screen_frame() above.
 *
 * This exists because this screen does not own the whole framebuffer the
 * way an LVGL screen with its own dirty-tracking does: whatever screen was
 * active before it (Info, today) leaves real pixels behind when it stops
 * being active, and this screen's own erase-then-draw per-frame contract
 * only ever erases its OWN previous 48x48 rectangle -- it has no idea
 * anything else on the field needs erasing too unless told. Skipping this
 * on entry means Home would only ever punch a moving 48x48 hole out of
 * whatever Info last drew, forever -- the same failure shape as the
 * black-trail bug ADR 0017 already hit once, for a different reason. See
 * docs/architecture/adr-0017-pet-screen.md:143-188. */
void kf_creature_screen_enter(void);

#endif /* KF_CREATURE_SCREEN_H */
