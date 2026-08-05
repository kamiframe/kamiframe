/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private state shared between the SDL display, input, and debug
 * window backends. Not visible to core.
 */

#ifndef KF_SDL_SHARED_H
#define KF_SDL_SHARED_H

#include <SDL3/SDL.h>

struct KfSdlState {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    int scale = 3;
    bool quit_requested = false;

    /* The debug window (sdl_debug_window.cpp), if it has been created --
     * nullptr until kf_sdl_debug_window_init() runs, and nullptr again
     * once closed (see debug_window_close_requested below; the debug
     * window is not recreated once closed, the same "closing it is final
     * for this session" simplicity kf_lua_port_init()'s own one-shot
     * failure paths already accept elsewhere in this codebase).
     *
     * Exposed here, not kept private inside sdl_debug_window.cpp, so the
     * ONE central event pump below (kf_sdl_pump_events()) can tell the two
     * windows' close buttons apart -- see that function's own comment on
     * why event polling has to stay centralized in the first place. */
    SDL_Window *debug_window = nullptr;

    /* Set by kf_sdl_pump_events() when the debug window's own close
     * button was clicked this frame; checked and cleared by
     * kf_sdl_debug_window_frame(). Deliberately separate from
     * quit_requested above -- closing the debug window must not quit the
     * whole simulator, only the main pet window's close button does
     * that. */
    bool debug_window_close_requested = false;
};

KfSdlState &kf_sdl_state(void);

/* Drain the SDL event queue. Called once per frame, from exactly one
 * place (kf_input_poll(), sdl_input.cpp) -- SDL_PollEvent() drains a
 * single process-wide queue, not a per-window one, so a second caller
 * polling independently (e.g. the debug window handling its own close
 * button) would race the first for events rather than each seeing the
 * ones meant for it. Everything that cares about an SDL event, across
 * however many windows exist, has to learn about it through state this
 * function sets rather than by polling on its own -- quit_requested and
 * debug_window_close_requested above are exactly that state. */
void kf_sdl_pump_events(void);

/* Mouse position relative to `window`'s own top-left corner, plus whether
 * the left button is currently held -- computed from SDL_GetGlobalMouseState()
 * (screen coordinates, unaffected by which window currently has input
 * focus) and the window's own on-screen position, rather than
 * SDL_GetMouseState() (which reports coordinates relative to whichever
 * window currently has mouse focus). That distinction only matters once a
 * second window exists: with a single window, the two are
 * indistinguishable, which is why sdl_input.cpp's original pointer
 * implementation used the simpler call and was correct at the time.
 *
 * Returns false, with *pressed forced to false, if the pointer is not
 * currently over `window` at all -- so a click landing in the debug
 * window is never misread as a click at some stale leftover coordinate
 * in the pet window, or vice versa. `x`/`y` are still written (clamped
 * into the window) even when returning false, so a caller that ignores
 * the return value degrades to "not pressed" rather than reading garbage. */
bool kf_sdl_mouse_relative_to(SDL_Window *window, int32_t *x, int32_t *y,
                               bool *pressed);

#endif /* KF_SDL_SHARED_H */
