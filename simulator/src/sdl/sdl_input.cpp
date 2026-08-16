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
 * "walk up" (it plays with the creature; see kf_home_screen_input.cpp's
 * kf_home_screen_handle_care_buttons()) does not read as discoverable. He
 * asked for number keys explicitly and said rewiring the keyboard was
 * fine, so this moves the five care actions off the D-pad/A entirely
 * rather than adding numbers alongside them -- one mapping to learn,
 * matching the on-screen guide (the five "N:ACTION" text objects
 * examples/creature_demo/creature.lua's kf.screen("home") group declares,
 * one per care action -- see that script's own `guide` table) key for
 * key, not two that happen to overlap.
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
 * Order matches kf_home_screen_handle_care_buttons()'s own mapping
 * (kf_home_screen_input.cpp) and the brief's original A/UP/DOWN/LEFT/RIGHT
 * order: feed, play, rest, bath, flush. */
constexpr Binding kBindings[] = {
    {SDL_SCANCODE_1, KF_BTN_A},         {SDL_SCANCODE_2, KF_BTN_UP},
    {SDL_SCANCODE_3, KF_BTN_DOWN},      {SDL_SCANCODE_4, KF_BTN_LEFT},
    {SDL_SCANCODE_5, KF_BTN_RIGHT},
    {SDL_SCANCODE_X, KF_BTN_B},         {SDL_SCANCODE_K, KF_BTN_B},
    {SDL_SCANCODE_RETURN, KF_BTN_MENU}, {SDL_SCANCODE_ESCAPE, KF_BTN_MENU},

    /* The nine handheld-layout buttons on the keyboard too, so a game using
     * them is still testable with no pad plugged in -- which is the whole
     * reason the keyboard bindings exist at all.
     *
     * Arrow keys join the d-pad here: 2-5 stay as they were because they
     * double as the care actions (see this file's header), but nobody
     * playing a side-scroller wants to steer with the number row.
     *
     * WASD-adjacent letters for the face buttons, the bracket keys for the
     * shoulders (they sit in a row, like shoulders do), and Tab/Backspace for
     * select/power -- deliberately awkward for power, since it is not
     * something to hit by accident. */
    {SDL_SCANCODE_UP, KF_BTN_UP},       {SDL_SCANCODE_DOWN, KF_BTN_DOWN},
    {SDL_SCANCODE_LEFT, KF_BTN_LEFT},   {SDL_SCANCODE_RIGHT, KF_BTN_RIGHT},
    {SDL_SCANCODE_Z, KF_BTN_A},         {SDL_SCANCODE_C, KF_BTN_X},
    {SDL_SCANCODE_V, KF_BTN_Y},
    {SDL_SCANCODE_Q, KF_BTN_L1},        {SDL_SCANCODE_E, KF_BTN_R1},
    {SDL_SCANCODE_LEFTBRACKET, KF_BTN_L2},
    {SDL_SCANCODE_RIGHTBRACKET, KF_BTN_R2},
    {SDL_SCANCODE_SPACE, KF_BTN_START}, {SDL_SCANCODE_TAB, KF_BTN_SELECT},
    {SDL_SCANCODE_BACKSPACE, KF_BTN_POWER},
};

/* GAMEPAD BINDINGS, alongside the keyboard rather than instead of it.
 *
 * SDL's GAMEPAD api, not its raw joystick one, and that choice is the whole
 * reason this is short: SDL ships a controller database and reports every
 * recognised pad through one abstract layout, so an 8BitDo, an Xbox pad and a
 * DualSense all arrive here as the same buttons without a line of
 * per-controller code. A raw joystick would hand us numbered buttons whose
 * meaning differs per device, which is how projects end up with a mapping
 * screen nobody wanted to write.
 *
 * The device has 7 buttons today and a real handheld layout is coming (a
 * d-pad, ABXY, shoulders, start/select/menu -- 16 of them, on an I2C
 * expander). These bindings deliberately map the pad's OWN d-pad and face
 * buttons onto the 7 that exist, so the mapping stays obvious when the rest
 * arrive rather than needing rethinking.
 *
 * BOTH FACE-BUTTON CONVENTIONS ARE ACCEPTED for menu confirm/cancel: SDL
 * reports positions, and Nintendo-layout pads (which the 8BitDo Ultimate is,
 * in Switch mode) have A and B physically swapped relative to Xbox ones. A
 * simulator that only honoured one would feel broken on the other, and the
 * distinction does not exist on the real hardware at all. */
struct PadBinding {
    SDL_GamepadButton pad;
    kf_button button;
};

constexpr PadBinding kPadBindings[] = {
    /* D-pad straight through. */
    {SDL_GAMEPAD_BUTTON_DPAD_UP, KF_BTN_UP},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, KF_BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, KF_BTN_LEFT},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, KF_BTN_RIGHT},

    /* FACE BUTTONS BY POSITION, which is what SDL reports and what a player
     * actually feels under their thumb.
     *
     * SDL names them SOUTH/EAST/WEST/NORTH rather than A/B/X/Y precisely
     * because the letters move: an Xbox pad reads A,B,X,Y clockwise from the
     * bottom, a Nintendo-layout pad -- which the 8BitDo Ultimate is in Switch
     * mode -- has A and B swapped and X and Y swapped against that. Binding
     * by POSITION means the bottom button is always the device's A on every
     * pad, which is the thing a player's muscle memory is actually attached
     * to. The alternative, honouring printed letters, would put "A" in a
     * different physical place depending on the controller. */
    {SDL_GAMEPAD_BUTTON_SOUTH, KF_BTN_A},
    {SDL_GAMEPAD_BUTTON_EAST, KF_BTN_B},
    {SDL_GAMEPAD_BUTTON_WEST, KF_BTN_X},
    {SDL_GAMEPAD_BUTTON_NORTH, KF_BTN_Y},

    /* Shoulders. L2/R2 are ANALOGUE triggers on most pads including this
     * one, so they are handled as axes below rather than here -- SDL only
     * reports them as buttons on pads that have digital ones. Both paths set
     * the same bit, so a pad with either kind works. */
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, KF_BTN_L1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, KF_BTN_R1},

    {SDL_GAMEPAD_BUTTON_START, KF_BTN_START},
    {SDL_GAMEPAD_BUTTON_BACK, KF_BTN_SELECT},

    /* The pad's home/guide key is the closest thing to a power button, and
     * mapping it there means a game can be written against POWER on desktop.
     * Note the real device's power button is a wake pin on its own GPIO, not
     * on the button expander -- see kf/types.h. */
    {SDL_GAMEPAD_BUTTON_GUIDE, KF_BTN_POWER},

    /* MENU has no dedicated pad control -- a physical pad has Start and
     * Select and that is it. Clicking the left stick is an unused, easily
     * reachable third option, and Enter/Esc still work on the keyboard. */
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, KF_BTN_MENU},
};

/* The first connected pad, or nullptr. One is enough: the device has one
 * player and no notion of a second. Opened and closed by the ADDED/REMOVED
 * events below rather than polled for, so hot-plugging works -- plugging a
 * controller in after the simulator started is the normal case, not the
 * exception. */
SDL_Gamepad *g_pad = nullptr;

/* Left stick as a d-pad, because a stick that does nothing feels broken even
 * when the d-pad works. The threshold is deliberately high (about 50% of
 * full deflection): this is a digital device with no analogue input
 * anywhere, so a low threshold would turn a resting thumb into held
 * directions. Hall-effect sticks like the 8BitDo Ultimate's barely drift,
 * but plenty of pads do, and the whole point of a simulator is that it
 * behaves like the hardware rather than like whatever is plugged into the
 * PC. */
constexpr int16_t kStickThreshold = 16000;

uint32_t pad_mask() {
    if (g_pad == nullptr) {
        return 0u;
    }
    uint32_t mask = 0u;
    for (const PadBinding &b : kPadBindings) {
        if (SDL_GetGamepadButton(g_pad, b.pad)) {
            mask |= static_cast<uint32_t>(b.button);
        }
    }
    const int16_t lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX);
    const int16_t ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (lx <= -kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_LEFT);
    } else if (lx >= kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_RIGHT);
    }
    if (ly <= -kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_UP);
    } else if (ly >= kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_DOWN);
    }

    /* Analogue triggers as digital L2/R2. SDL reports these on 0..32767
     * (they only travel one way), and half-pull is the conventional point to
     * call a trigger pressed. The device will have plain switches here, so
     * flattening to on/off is not a simplification -- it is the behaviour. */
    if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >=
        kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_L2);
    }
    if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >=
        kStickThreshold) {
        mask |= static_cast<uint32_t>(KF_BTN_R2);
    }
    return mask;
}

} // namespace

void kf_sdl_pump_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            kf_sdl_state().quit_requested = true;
        } else if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            /* Take the first pad and ignore the rest. Logged by name because
             * "my controller does nothing" is otherwise indistinguishable
             * from "SDL never recognised it", and those need different
             * fixes. */
            if (g_pad == nullptr) {
                g_pad = SDL_OpenGamepad(event.gdevice.which);
                if (g_pad != nullptr) {
                    const char *name = SDL_GetGamepadName(g_pad);
                    KF_LOGI(TAG, "gamepad connected: %s -- d-pad/left stick "
                                 "= directions, A/B = A, B/X = B, "
                                 "Start/Back = menu",
                            name != nullptr ? name : "(unnamed)");
                } else {
                    KF_LOGW(TAG, "a gamepad was connected but SDL could not "
                                 "open it: %s", SDL_GetError());
                }
            }
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (g_pad != nullptr &&
                event.gdevice.which == SDL_GetGamepadID(g_pad)) {
                SDL_CloseGamepad(g_pad);
                g_pad = nullptr;
                KF_LOGI(TAG, "gamepad disconnected -- keyboard still works");
            }
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
    KF_LOGI(TAG, "keyboard: 1-5 = feed/play/rest/bath/flush, arrows = d-pad, "
                 "Z=A X/K=B C=X V=Y, Q=L1 E=R1 [=L2 ]=R2, Space=start "
                 "Tab=select Enter/Esc=menu Backspace=power");
    /* Not fatal if it fails: a simulator with no gamepad subsystem is a
     * simulator with a keyboard, which is how every session before this one
     * worked. Says so rather than failing silently. */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        KF_LOGW(TAG, "gamepad support unavailable (%s) -- keyboard only",
                SDL_GetError());
        return KF_OK;
    }
    /* Pads already plugged in at startup do not generate ADDED events, so
     * they have to be found once here. Everything after this arrives as an
     * event. */
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids != nullptr) {
        for (int i = 0; i < count && g_pad == nullptr; ++i) {
            g_pad = SDL_OpenGamepad(ids[i]);
            if (g_pad != nullptr) {
                const char *name = SDL_GetGamepadName(g_pad);
                KF_LOGI(TAG, "gamepad: %s -- d-pad/left stick = directions, "
                             "A/B = A, B/X = B, Start/Back = menu",
                        name != nullptr ? name : "(unnamed)");
            }
        }
        SDL_free(ids);
    }
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

    /* Keyboard OR gamepad, not one instead of the other -- both feed the
     * same mask, so a game cannot tell which was used and neither can the
     * pet. That is the point: this is the same kf_input_raw the real
     * buttons produce on device. */
    mask |= pad_mask();

    out->buttons = mask;
    out->sampled_at_us = kf_time_mono_us();
    out->quit_requested = kf_sdl_state().quit_requested;
    return KF_OK;
}

void kf_input_shutdown(void) {
    if (g_pad != nullptr) {
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
    }
}

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
