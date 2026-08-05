/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Switches which LVGL screen is on top -- Home (kf_pet_screen.cpp) or Info
 * (kf_pet_info_screen.cpp) so far, more later -- and owns the ONE input
 * mapping that does it: MENU advances to the next screen, wrapping back to
 * Home; B jumps straight back to Home from anywhere. See ADR 0022.
 *
 * Deliberately reads kf_app_buttons_pressed() directly, the same debounced
 * edge state kf/app.cpp's own KF_BTN_MENU-toggles-the-HUD code reads --
 * NOT through kf_lvgl_input.cpp's keypad indev. Screen switching and LVGL's
 * own keypad-group focus system are kept orthogonal on purpose: which
 * screen is loaded has nothing to do with which widget inside that screen
 * currently has keypad focus, and mixing the two would mean MENU sometimes
 * cycles focus and sometimes changes screens depending on what LVGL's
 * group state happened to be -- see kf_lvgl_input.cpp's own header comment
 * for why MENU no longer feeds LV_KEY_NEXT there at all now that this file
 * owns it exclusively. This also means a screen genuinely does not need an
 * lv_group_t to be navigable -- kf_pet_info_screen.cpp has none -- since
 * nothing here ever asks LVGL's group system to do anything.
 */

#ifndef KF_SCREEN_NAV_H
#define KF_SCREEN_NAV_H

/* Brings up every screen this build knows about (currently: Home, Info)
 * and loads Home first. Call once, after kf_lvgl_port_init() and
 * kf_pet_session_init() -- both screens' init functions need the pet
 * session ready the same way kf_pet_screen_init() always has. Replaces a
 * direct kf_pet_screen_init() call in sdl_main.cpp; nothing outside this
 * file should call kf_pet_screen_init()/kf_pet_info_screen_init() directly
 * any more, except headless_main.cpp's own pet_screen_check, which
 * deliberately keeps exercising kf_pet_screen.cpp in isolation the way it
 * always has (see that file's own comment) and is unaffected by anything
 * in this file. */
void kf_screen_nav_init(void);

/* Reads this frame's MENU/B edges and switches screens if either fired,
 * then calls whichever screen is now active's own *_update() function --
 * only the active one; the inactive screen's widgets sit untouched until
 * it is shown again, the same "why redraw what nobody can see" reasoning
 * kf_pet_screen.h's own per-frame contract already follows. Call once per
 * frame, in the exact same slot sdl_main.cpp used to call
 * kf_pet_screen_update() directly -- after kf_pet_session_frame() has
 * applied this frame's elapsed time, before kf_lvgl_port_pump() redraws
 * and flushes. See ADR 0017's frame-ordering note, still the same
 * requirement now that a second screen exists. */
void kf_screen_nav_frame(void);

/* ---------------------------------------------------------------------
 * DEBUG/TEST ONLY below this line, the same status kf_pet_session.h's own
 * "DEBUG ONLY" section has: not part of the gameplay surface, not called
 * by the interactive build. Their purpose is letting a headless check (or,
 * later, a debug-window button) drive screen switching directly rather
 * than fighting headless_input.cpp's single shared, frame-indexed button
 * script -- see headless_main.cpp's run_screen_nav_check() for the actual
 * caller. Same effect as a real MENU/B press, just callable without one.
 * --------------------------------------------------------------------- */

/* Same effect as a MENU edge: advances to the next screen, wrapping back
 * to Home. */
void kf_screen_nav_debug_advance(void);

/* Same effect as a B edge: jumps straight back to Home. A no-op if Home is
 * already active, matching the real button's behaviour. */
void kf_screen_nav_debug_home(void);

/* Which screen is active right now: 0 = Home, 1 = Info. For test
 * assertions only -- nothing in the interactive build reads this. */
int kf_screen_nav_debug_index(void);

#endif /* KF_SCREEN_NAV_H */
