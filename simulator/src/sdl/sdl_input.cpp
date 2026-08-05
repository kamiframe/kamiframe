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
 * Keyboard mapping matches the 5-7 button target hardware:
 *
 *     arrows / WASD   D-pad
 *     Z / J           A
 *     X / K           B
 *     Enter / Escape  Menu
 */

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

constexpr Binding kBindings[] = {
    {SDL_SCANCODE_UP, KF_BTN_UP},       {SDL_SCANCODE_W, KF_BTN_UP},
    {SDL_SCANCODE_DOWN, KF_BTN_DOWN},   {SDL_SCANCODE_S, KF_BTN_DOWN},
    {SDL_SCANCODE_LEFT, KF_BTN_LEFT},   {SDL_SCANCODE_A, KF_BTN_LEFT},
    {SDL_SCANCODE_RIGHT, KF_BTN_RIGHT}, {SDL_SCANCODE_D, KF_BTN_RIGHT},
    {SDL_SCANCODE_Z, KF_BTN_A},         {SDL_SCANCODE_J, KF_BTN_A},
    {SDL_SCANCODE_X, KF_BTN_B},         {SDL_SCANCODE_K, KF_BTN_B},
    {SDL_SCANCODE_RETURN, KF_BTN_MENU}, {SDL_SCANCODE_ESCAPE, KF_BTN_MENU},
};

} // namespace

void kf_sdl_pump_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            kf_sdl_state().quit_requested = true;
        }
    }
}

kf_result kf_input_init(void) {
    KF_LOGI(TAG, "keyboard: arrows/WASD = d-pad, Z/J = A, X/K = B, "
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
 * them (kf_input_raw). SDL reports window pixel coordinates; the pet
 * screen's widgets are laid out in the logical 240x320 framebuffer space
 * (KF_DISPLAY_WIDTH x KF_DISPLAY_HEIGHT), so this divides by the same
 * integer `scale` sdl_display.cpp used to size the window in the first
 * place -- the exact inverse of that multiplication, not a separate
 * assumption about window size. */
void kf_sim_pointer_poll(int32_t *x, int32_t *y, bool *pressed) {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mouse_x, &mouse_y);

    const int scale = kf_sdl_state().scale > 0 ? kf_sdl_state().scale : 1;
    *x = static_cast<int32_t>(mouse_x) / scale;
    *y = static_cast<int32_t>(mouse_y) / scale;
    *pressed = (buttons & SDL_BUTTON_LMASK) != 0u;
}
