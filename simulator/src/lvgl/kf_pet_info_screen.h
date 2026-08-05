/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The second real screen (ADR 0022) -- read-only pet identity/progress,
 * as a separate LVGL screen object from kf_pet_screen.cpp's Home screen
 * (which owns needs + care actions). Reached via kf_screen_nav.h, not
 * called directly by sdl_main.cpp -- see that file's header comment for
 * why navigation is a separate module from either screen.
 *
 * Deliberately not a debug tool: everything shown here is drawn from
 * `kf_pet_state`'s public fields only (state->stage,
 * state->stage_elapsed_seconds, state->teen_form/adult_branch), never
 * kf_pet_session.h's "DEBUG ONLY" kf_pet_session_debug_age_seconds() --
 * that accessor's own header comment reserves it for the simulator's
 * debug window, not gameplay, and total-lifetime age additionally needs
 * summed-up config durations that live only as an internal helper in
 * kf_pet_session.cpp. Time IN THE CURRENT STAGE is public state already
 * (no config lookup needed) and answers the question a player actually
 * has -- "how long has my pet been like this" -- without either problem.
 *
 * No interactive widgets, no LVGL group: matching hardware with no
 * touchscreen (kf_lvgl_input.h's own header comment), this screen has
 * nothing on it a button would focus, so there is nothing to register --
 * see kf_screen_nav.cpp's header comment for why screens don't need a
 * group at all to be navigable.
 */

#ifndef KF_PET_INFO_SCREEN_H
#define KF_PET_INFO_SCREEN_H

#include <lvgl.h>

/* Builds this screen's widgets onto a NEW lv_obj_t screen (lv_obj_create
 * (nullptr)) -- does not touch or load the currently active screen, so
 * calling this does not disturb kf_pet_screen.cpp's Home screen, whichever
 * one happens to be active at the time. Call once, after kf_lvgl_port_init()
 * and kf_pet_session_init(). Calls kf_pet_info_screen_update() once itself
 * at the end, the same "real values from the first frame" convention
 * kf_pet_screen_init() already uses. */
void kf_pet_info_screen_init(void);

/* Pushes the live kf_pet_session_state() into this screen's labels. Call
 * once per frame while this screen is the active one -- see
 * kf_screen_nav.h's header comment for why only the active screen's update
 * function runs each frame, not both. Safe to call while this screen is NOT
 * loaded too (it only writes label text, LVGL does not render an unloaded
 * screen), kf_screen_nav.cpp just does not bother while Home is active. */
void kf_pet_info_screen_update(void);

/* The lv_obj_t this screen's widgets live on -- what kf_screen_nav.cpp
 * passes to lv_screen_load() to make this screen visible. Never NULL after
 * kf_pet_info_screen_init(). */
lv_obj_t *kf_pet_info_screen_root(void);

#endif /* KF_PET_INFO_SCREEN_H */
