/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: display, SDL3.
 *
 * Two decisions here are about honesty rather than convenience:
 *
 * 1. The texture is SDL_PIXELFORMAT_RGB565, the exact bytes the ST7789 would
 *    receive. No CPU-side conversion to RGBA. A conversion step would be
 *    desktop-only code with no device counterpart, and it would hide
 *    pixel-format bugs that hardware would show immediately.
 *
 * 2. Scaling is nearest-neighbour at an integer factor. Linear filtering
 *    would make sprites look better here than they can ever look on a 240x320
 *    panel, which is the same category of lie as running at 400fps.
 *
 * The dirty rectangles are accepted but not used to limit the upload: on a
 * GPU, uploading 153KB costs nothing worth optimising. The device backend
 * will use them, which is exactly why they are in the signature.
 */

#include "kf/hal/display.h"

#include "kf/budget.h"
#include "kf/hal/log.h"
#include "sdl_shared.h"

#include <SDL3/SDL.h>

namespace {

constexpr const char *TAG = "display";

KfSdlState g_state;

kf_display_caps g_caps = {
    KF_DISPLAY_WIDTH,
    KF_DISPLAY_HEIGHT,
    KF_PIXFMT_RGB565,
    /* supports_partial_update: false. The GPU redraws everything anyway, and
     * claiming otherwise would let core believe partial updates are already
     * being honoured. */
    false,
    /* has_backlight */ false,
    /* link_bytes_per_second: deliberately the DEVICE's figure, not the
     * host's. The host has no meaningful link speed, and reporting zero would
     * silently switch off the transfer-cost estimate, which is the one number
     * desktop cannot measure and most needs to show. */
    KF_DISPLAY_SPI_HZ / 8u,
};

} // namespace

KfSdlState &kf_sdl_state(void) { return g_state; }

kf_result kf_display_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        KF_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }

    const int win_w = KF_DISPLAY_WIDTH * g_state.scale;
    const int win_h = KF_DISPLAY_HEIGHT * g_state.scale;

    if (!SDL_CreateWindowAndRenderer("Kamiframe simulator (HakoniwaOS)", win_w,
                                     win_h, 0, &g_state.window,
                                     &g_state.renderer)) {
        KF_LOGE(TAG, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }

    g_state.texture =
        SDL_CreateTexture(g_state.renderer, SDL_PIXELFORMAT_RGB565,
                          SDL_TEXTUREACCESS_STREAMING, KF_DISPLAY_WIDTH,
                          KF_DISPLAY_HEIGHT);
    if (g_state.texture == nullptr) {
        KF_LOGE(TAG, "SDL_CreateTexture(RGB565) failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }

    SDL_SetTextureScaleMode(g_state.texture, SDL_SCALEMODE_NEAREST);

    KF_LOGI(TAG, "SDL3 window %dx%d (%dx scale), RGB565 streaming texture",
            win_w, win_h, g_state.scale);
    return KF_OK;
}

const kf_display_caps *kf_display_get_caps(void) { return &g_caps; }

kf_result kf_display_present(const kf_color *framebuffer,
                              const kf_rect *dirty_rects, int dirty_rect_count) {
    (void)dirty_rects;
    (void)dirty_rect_count;

    if (g_state.texture == nullptr) {
        return KF_ERR_UNAVAILABLE;
    }

    const int pitch = KF_DISPLAY_WIDTH * static_cast<int>(sizeof(kf_color));
    if (!SDL_UpdateTexture(g_state.texture, nullptr, framebuffer, pitch)) {
        KF_LOGE(TAG, "SDL_UpdateTexture failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }

    SDL_RenderClear(g_state.renderer);
    SDL_RenderTexture(g_state.renderer, g_state.texture, nullptr, nullptr);
    SDL_RenderPresent(g_state.renderer);
    return KF_OK;
}

kf_result kf_display_set_backlight(uint8_t level) {
    (void)level;
    return KF_ERR_UNAVAILABLE;
}

void kf_display_shutdown(void) {
    if (g_state.texture != nullptr) {
        SDL_DestroyTexture(g_state.texture);
        g_state.texture = nullptr;
    }
    if (g_state.renderer != nullptr) {
        SDL_DestroyRenderer(g_state.renderer);
        g_state.renderer = nullptr;
    }
    if (g_state.window != nullptr) {
        SDL_DestroyWindow(g_state.window);
        g_state.window = nullptr;
    }
    SDL_Quit();
}
