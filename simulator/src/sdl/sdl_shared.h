/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private state shared between the SDL display and input backends.
 * Not visible to core.
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
};

KfSdlState &kf_sdl_state(void);

/* Drain the SDL event queue. Called once per frame by the input backend;
 * the display backend relies on it having happened. */
void kf_sdl_pump_events(void);

#endif /* KF_SDL_SHARED_H */
