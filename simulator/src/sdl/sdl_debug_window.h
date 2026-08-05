/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * A second SDL window, entirely separate from the pet window, for
 * interactive development controls -- time-skip buttons, a live stats
 * readout, a play-speed multiplier -- so none of it has to sit on the
 * actual pet screen (kf_pet_screen.cpp), which is meant to represent what
 * a real device would show. This window has no hardware counterpart and
 * never will, the same status as `--stress` mode and the window title
 * stats (sdl_main.cpp's update_title()) already have.
 *
 * Deliberately plain SDL drawing (rects, SDL_RenderDebugText), not LVGL --
 * a debug panel of buttons and numbers does not need LVGL's widget/layout
 * machinery, and pulling in a second full LVGL display instance just for
 * this would be real added complexity (a second display driver, a second
 * input device, its own arena budget) for something this simple, the same
 * "use the plainest tool that solves it" call ADR 0010 already made for
 * the on-device HUD's hand-rolled bitmap font instead of a real font
 * library. This window is desktop-simulator-only in a stronger sense than
 * even the pet screen: it will never be ported anywhere, so there is no
 * portability cost being traded away here, only genuinely unneeded
 * complexity avoided.
 *
 * Two columns as of the screen-nav follow-up (ADR 0022): the original
 * controls on the left, unchanged, plus a "Next Screen" button that calls
 * kf_screen_nav_debug_advance() directly -- the same effect a real MENU
 * keypress has on which pet-window screen is loaded, but WITHOUT also
 * flipping Core's on-device constraint HUD (kf/app.cpp's draw_hud(),
 * toggled on every KF_BTN_MENU edge by design, see ADR 0010) on top of
 * whichever screen is now showing. That collision -- switching screens by
 * pressing MENU also toggling a HUD that draws text over the pet window --
 * is real, reported directly by Chris after ADR 0022 shipped, and made
 * worse by MENU now being something he presses far more often than it was
 * in ADR 0017's single-screen build. The keyboard's Enter/Esc still does
 * both at once, unchanged -- that IS what a real device does when its one
 * MENU button is pressed, and this file does not get to quietly change
 * that (ADR 0006/0010 are Core, not simulator-only, compiled identically
 * for ESP32). The new button is a second, additive path that never
 * touches KF_BTN_MENU at all, so it never triggers the HUD. A new
 * right-hand column mirrors what the on-device HUD would show (frame
 * timing, dirty area, arena high-water marks) by calling the exact same
 * public accessors kf/app.cpp's draw_hud() itself reads
 * (kf_app_last_frame(), kf_app_frame_summary(), kf_arena_get_stats()) --
 * so that information is visible here, permanently, without ever needing
 * to toggle the real overlay at all during ordinary interactive use.
 */

#ifndef KF_SDL_DEBUG_WINDOW_H
#define KF_SDL_DEBUG_WINDOW_H

#include <cstdint>

/* Creates the debug window, positioned to the right of the pet window.
 * Call once, after kf_display_init() (the pet window must already exist
 * to position this one relative to it) and after kf_pet_session_init()
 * (its buttons call straight into kf_pet_session_debug_*()). */
void kf_sdl_debug_window_init(void);

/* Polls this window's own clicks and drags -- multiplier/action buttons,
 * plus the timeline's draggable scrub head -- via sdl_shared.h's
 * kf_sdl_mouse_relative_to(), the same way kf_lvgl_pointer.cpp uses it
 * for the pet window, and redraws it. Call once per frame. A no-op once
 * the debug window has been closed (see sdl_shared.h's debug_window_
 * close_requested) -- it is not recreated automatically. */
void kf_sdl_debug_window_frame(void);

/* The play-speed multiplier currently selected on the debug window (one
 * of kButtons' kMult* values, sdl_debug_window.cpp -- 1 through 256) --
 * see sdl_main.cpp's main loop for the only current reader. 1 (real
 * time, unscaled) until a multiplier button is clicked, and again if the
 * debug window is never created at all (a plain `kamiframe-sim` build
 * always behaves exactly as it did before this file existed). */
uint32_t kf_sdl_debug_window_time_multiplier(void);

void kf_sdl_debug_window_shutdown(void);

#endif /* KF_SDL_DEBUG_WINDOW_H */
