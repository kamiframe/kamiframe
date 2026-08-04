/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Real content, not a proof screen -- see docs/architecture/
 * adr-0017-pet-screen.md. Draws the three live needs from
 * kf_pet_session_state() (ADR 0016) as three bars with live percentage
 * labels, and three buttons that call kf_pet_session_feed()/play()/rest()
 * when pressed.
 *
 * kf_lvgl_proof_screen.h's own header comment named this exact moment:
 * "delete it once real menu screens exist." This is that real screen for
 * the interactive build (see sdl_main.cpp) -- the proof screen itself is
 * deliberately left in place rather than deleted, because
 * lvgl_determinism_check's existing golden-checksum regression test still
 * exercises it, and touching an already-verified check to chase a cleanup
 * was not worth the risk for this slice. See the ADR's "Decision" section.
 */

#ifndef KF_PET_SCREEN_H
#define KF_PET_SCREEN_H

/* Builds the screen's widgets and registers the three buttons with LVGL's
 * default input group (see kf_lvgl_input.h) so the keypad bridge can reach
 * them: MENU cycles focus, A/ENTER activates whichever button is focused.
 *
 * Requires kf_pet_session_init() (kf_pet_session.h) to already have run --
 * the buttons this creates can call kf_pet_session_feed()/play()/rest()
 * from the moment a keypress reaches them, not lazily. Call once, after
 * kf_lvgl_port_init(). Calls kf_pet_screen_update() once itself at the
 * end, so the screen shows real values from its very first frame rather
 * than whatever LVGL's default zero-initialised bar state would be. */
void kf_pet_screen_init(void);

/* Pushes the live kf_pet_session_state() into the three bars and their
 * value labels. Call once per frame, after kf_pet_session_frame() has
 * applied that frame's elapsed time -- so what is drawn is the state as of
 * the frame that just ran, not the one before it -- and before
 * kf_lvgl_port_pump(), so the redraw that pump's lv_timer_handler() call
 * performs picks up this frame's values rather than waiting a frame
 * behind. */
void kf_pet_screen_update(void);

#endif /* KF_PET_SCREEN_H */
