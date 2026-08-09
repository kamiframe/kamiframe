/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: input, SDL3.
 *
 * Reports RAW key state only. No debounce, no repeat, no edge detection: core
 * does all of that (see debounce() in app.cpp) so that the simulator has the
 * same input latency and the same feel as the hardware. A backend that
 * "helpfully" smoothed input here would make the simulator feel better than
 * the device, which is the failure mode this whole architecture exists to
 * avoid.
 *
 * Keyboard mapping models the 5-7 button target hardware, EXCEPT for the
 * five care-action buttons (KF_BTN_A/UP/DOWN/LEFT/RIGHT), which this
 * simulator build reaches through number keys instead of arrows/WASD:
 *
 *     1 2 3 4 5       feed / play / rest / bath / flush
 *                     (KF_BTN_A / UP / DOWN / LEFT / RIGHT)
 *     X / K           B (jump to Home)
 *     Enter / Escape  Menu (advance screens)
 *
 * The project owner could not tell which key did what -- there is no
 * on-screen legend for arrows/WASD/Z, and "up" doing something that is not
 * "walk up" (it plays with the creature; see kf_creature_screen.cpp's
 * handle_care_buttons()) does not read as discoverable. He asked for
 * number keys explicitly and said rewiring the keyboard was fine, so this
 * moves the five care actions off the D-pad/A entirely rather than adding
 * numbers alongside them -- one mapping to learn, matching the on-screen
 * guide (kf_creature_screen.cpp's draw_care_guide()) key for key, not two
 * that happen to overlap.
 *
 * This is a DESKTOP-ONLY remap. The real device still has exactly seven
 * physical buttons and no number keys (kf/types.h's kf_button) -- what
 * changes here is only which host key produces which kf_button event, not
 * the button model itself (KF_BTN_A/UP/DOWN/LEFT/RIGHT/B/MENU are
 * untouched, see kf/types.h). A guide reading "1 = feed" is therefore
 * correct on this keyboard and would be wrong on the device; the
 * device-facing wording is a design decision for the project owner, not
 * settled here -- see this task's own report. */

#include "kf/hal/input.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "sdl_shared.h"

#include <SDL3/SDL.h>

namespace {

constexpr const char *TAG = "input";

struct Binding {
    SDL_Scancode scancode;
    kf_button button;
};

/* 1-5 replace arrows/WASD/Z/J entirely for the five care-action buttons --
 * see this file's header comment for why this is a move, not an addition.
 * Order matches handle_care_buttons()'s own mapping (kf_creature_screen.cpp)
 * and the brief's original A/UP/DOWN/LEFT/RIGHT order: feed, play, rest,
 * bath, flush. */
constexpr Binding kBindings[] = {
    {SDL_SCANCODE_1, KF_BTN_A},         {SDL_SCANCODE_2, KF_BTN_UP},
    {SDL_SCANCODE_3, KF_BTN_DOWN},      {SDL_SCANCODE_4, KF_BTN_LEFT},
    {SDL_SCANCODE_5, KF_BTN_RIGHT},
    {SDL_SCANCODE_X, KF_BTN_B},         {SDL_SCANCODE_K, KF_BTN_B},
    {SDL_SCANCODE_RETURN, KF_BTN_MENU}, {SDL_SCANCODE_ESCAPE, KF_BTN_MENU},
};

} // namespace

void kf_sdl_pump_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            kf_sdl_state().quit_requested = true;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            /* Two windows can exist now (sdl_debug_window.cpp) -- tell
             * their close buttons apart by SDL_WindowID rather than
             * assuming any close means quit, the assumption that was
             * correct back when this was the only window. */
            KfSdlState &s = kf_sdl_state();
            if (s.window != nullptr &&
                event.window.windowID == SDL_GetWindowID(s.window)) {
                s.quit_requested = true;
            } else if (s.debug_window != nullptr &&
                       event.window.windowID ==
                           SDL_GetWindowID(s.debug_window)) {
                s.debug_window_close_requested = true;
            }
        }
    }
}

bool kf_sdl_mouse_relative_to(SDL_Window *window, int32_t *x, int32_t *y,
                               bool *pressed) {
    float global_x = 0.0f;
    float global_y = 0.0f;
    const SDL_MouseButtonFlags buttons =
        SDL_GetGlobalMouseState(&global_x, &global_y);

    int win_x = 0;
    int win_y = 0;
    int win_w = 0;
    int win_h = 0;
    SDL_GetWindowPosition(window, &win_x, &win_y);
    SDL_GetWindowSize(window, &win_w, &win_h);

    const float rel_x = global_x - static_cast<float>(win_x);
    const float rel_y = global_y - static_cast<float>(win_y);
    *x = static_cast<int32_t>(rel_x);
    *y = static_cast<int32_t>(rel_y);

    const bool over_window = rel_x >= 0.0f && rel_x < static_cast<float>(win_w) &&
                              rel_y >= 0.0f && rel_y < static_cast<float>(win_h);
    *pressed = over_window && (buttons & SDL_BUTTON_LMASK) != 0u;
    return over_window;
}

kf_result kf_input_init(void) {
    KF_LOGI(TAG, "keyboard: 1-5 = feed/play/rest/bath/flush, X/K = B, "
                 "Enter/Esc = menu");
    return KF_OK;
}

kf_result kf_input_poll(kf_input_raw *out) {
    if (out == nullptr) {
        return KF_ERR_INVALID;
    }

    kf_sdl_pump_events();

    const bool *keys = SDL_GetKeyboardState(nullptr);

    uint32_t mask = 0;
    if (keys != nullptr) {
        for (const Binding &b : kBindings) {
            if (keys[b.scancode]) {
                mask |= static_cast<uint32_t>(b.button);
            }
        }
    }

    out->buttons = mask;
    out->sampled_at_us = kf_time_mono_us();
    out->quit_requested = kf_sdl_state().quit_requested;
    return KF_OK;
}

void kf_input_shutdown(void) {}

/* kf_lvgl_pointer.cpp's other half -- see that file's header comment. Not
 * part of kf/hal/input.h: a mouse pointer is not one of the real device's
 * 5-7 physical buttons, so it does not belong in the interface that models
 * them (kf_input_raw). Uses kf_sdl_mouse_relative_to() (sdl_shared.h), not
 * a direct SDL_GetMouseState() call, now that a second window
 * (sdl_debug_window.cpp) can exist -- SDL_GetMouseState()'s coordinates
 * are relative to whichever window currently has mouse focus, so a click
 * actually landing in the debug window would otherwise be reported here
 * too, at whatever the pet window's last coordinates happened to be. The
 * pet screen's widgets are laid out in the logical 240x320 framebuffer
 * space (KF_DISPLAY_WIDTH x KF_DISPLAY_HEIGHT), so this divides by the
 * same integer `scale` sdl_display.cpp used to size the window in the
 * first place -- the exact inverse of that multiplication, not a separate
 * assumption about window size. */
void kf_sim_pointer_poll(int32_t *x, int32_t *y, bool *pressed) {
    int32_t raw_x = 0;
    int32_t raw_y = 0;
    kf_sdl_mouse_relative_to(kf_sdl_state().window, &raw_x, &raw_y, pressed);

    const int scale = kf_sdl_state().scale > 0 ? kf_sdl_state().scale : 1;
    *x = raw_x / scale;
    *y = raw_y / scale;
}
