/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Placeholder slice-one content. See kf/demo.h.
 */

#include "kf/demo.h"

#include "kf/arena.h"
#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/rng.h"

#include <cstdint>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "demo";

/* Positions are in 1/16th pixels so movement is smooth without floating
 * point in the hot path. Sub-pixel integer motion is exactly reproducible
 * frame for frame, which is what makes the headless CI test meaningful. */
constexpr int32_t kSub = 16;

/* A conventional size for a 2D tile game. 240x320 needs 15x20 = 300 of them
 * to cover the screen, plus one extra row and column while scrolling. */
constexpr int kTileSize = 16;
constexpr int kTileCount = 8;
constexpr int kTilesAcross = (KF_DISPLAY_WIDTH / kTileSize) + 1;
constexpr int kTilesDown = (KF_DISPLAY_HEIGHT / kTileSize) + 1;

constexpr int kStressSprites = 12;

struct Mover {
    int32_t x, y, vx, vy;
};

struct Demo {
    kf_demo_mode mode = KF_DEMO_SPRITE;

    /* Value-initialised, then frame_count explicitly forced to 1 by name
     * rather than by position, so this sprite meets its own type's
     * invariant ("always >= 1", kf/types.h) even before kf_demo_init()'s
     * `sprite = *test_sprite` overwrites every field wholesale. This has to
     * survive a field being inserted anywhere in kf_sprite: C++17 has no
     * designated initialisers, and a positional aggregate initialiser
     * silently reassigns which member "1" lands on when the struct's shape
     * changes -- a shorter-than-full-width list is legal C++, so the
     * compiler gives no warning either. Nothing reads this particular
     * frame_count today, but a value that violates the type it belongs to
     * is a bug waiting for the day something does. */
    kf_sprite sprite = [] {
        kf_sprite s{};
        s.frame_count = 1;
        return s;
    }();
    Mover movers[kStressSprites];
    int mover_count = 1;

    kf_color background = 0;

    /* Tileset, generated at init straight into KF_ARENA_ASSETS -- procedural
     * game data, not a packed sprite, so it has no business going through
     * the asset pipeline (kf/assets.h) the real sprite above now comes
     * from. Kept as-is since ADR 0033: it is exactly the kind of "decoded
     * sprites and game data" kf/arena.h's own comment says that arena is
     * for, alongside the asset pipeline's own small descriptor table. */
    kf_sprite tiles[kTileCount];
    int32_t scroll_x = 0;
    int32_t scroll_y = 0;

    /* Where the sprite was last frame, so KF_DEMO_SPRITE can repaint just
     * that patch instead of the whole screen. */
    kf_rect previous = {0, 0, 0, 0};
    bool needs_full_repaint = true;
};

Demo d;

kf_rect sprite_rect(const Mover &m) {
    const int16_t x = static_cast<int16_t>(m.x / kSub);
    const int16_t y = static_cast<int16_t>(m.y / kSub);
    return kf_rect{x, y, static_cast<int16_t>(x + d.sprite.width),
                   static_cast<int16_t>(y + d.sprite.height)};
}

void build_tileset(void) {
    for (int t = 0; t < kTileCount; ++t) {
        kf_color *pixels = static_cast<kf_color *>(kf_arena_alloc(
            KF_ARENA_ASSETS,
            sizeof(kf_color) * kTileSize * kTileSize, 4));

        const uint8_t base_r = static_cast<uint8_t>(30 + t * 9);
        const uint8_t base_g = static_cast<uint8_t>(60 + ((t * 23) % 120));
        const uint8_t base_b = static_cast<uint8_t>(90 + ((t * 41) % 110));

        for (int y = 0; y < kTileSize; ++y) {
            for (int x = 0; x < kTileSize; ++x) {
                /* A border and a little interior texture, so scrolling is
                 * visible rather than a flat wash. */
                const bool edge = (x == 0 || y == 0 || x == kTileSize - 1 ||
                                   y == kTileSize - 1);
                const int speck = ((x * 7 + y * 13 + t * 5) % 11 == 0) ? 26 : 0;
                const int shade = edge ? -22 : speck;

                const int r = base_r + shade;
                const int gg = base_g + shade;
                const int b = base_b + shade;
                pixels[y * kTileSize + x] =
                    KF_RGB(static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r)),
                           static_cast<uint8_t>(gg < 0 ? 0 : (gg > 255 ? 255 : gg)),
                           static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b)));
            }
        }

        d.tiles[t].pixels = pixels;
        d.tiles[t].width = kTileSize;
        d.tiles[t].height = kTileSize;
        d.tiles[t].color_key = 0;
        /* Opaque on purpose: a background tile has nothing behind it, and an
         * opaque blit is a per-row memcpy instead of a per-pixel test. That
         * difference is roughly 4x on the device. */
        d.tiles[t].has_color_key = false;
    }
}

uint8_t tile_at(int col, int row) {
    /* Deterministic pseudo-random map. No allocation, no state. */
    /* Unsigned throughout: signed integer overflow is undefined behaviour,
     * and a hash function is exactly where it would happen. */
    const uint32_t h = ((static_cast<uint32_t>(col) * 374761393u) ^
                        (static_cast<uint32_t>(row) * 668265263u)) *
                       1274126177u;
    return static_cast<uint8_t>((h >> 13) % kTileCount);
}

void draw_tilemap(void) {
    const int px = static_cast<int>(d.scroll_x / kSub);
    const int py = static_cast<int>(d.scroll_y / kSub);

    /* Which tile the top-left of the screen falls inside, and how far into
     * it, so the map scrolls smoothly rather than jumping a tile at a time. */
    int col0 = px / kTileSize;
    int row0 = py / kTileSize;
    int off_x = px % kTileSize;
    int off_y = py % kTileSize;
    if (off_x < 0) {
        off_x += kTileSize;
        col0--;
    }
    if (off_y < 0) {
        off_y += kTileSize;
        row0--;
    }

    for (int row = 0; row < kTilesDown; ++row) {
        for (int col = 0; col < kTilesAcross; ++col) {
            const uint8_t index = tile_at(col0 + col, row0 + row);
            kf_blit(&d.tiles[index],
                    static_cast<int16_t>(col * kTileSize - off_x),
                    static_cast<int16_t>(row * kTileSize - off_y));
        }
    }
}

} // namespace

void kf_demo_init(uint32_t seed, kf_demo_mode mode) {
    kf_rng_seed(seed);
    d.mode = mode;

    if (mode == KF_DEMO_NONE) {
        /* Nothing to set up: no sprite moves, no tileset gets built, no
         * pixel of the framebuffer is ever touched by this file again --
         * see kf_demo_update()/kf_demo_draw()'s own early returns below.
         * Whatever owns the screen (LVGL, as of ADR 0017) owns all of it,
         * uncontested -- see kf/demo.h's own comment on KF_DEMO_NONE for
         * why that turned out to matter. */
        KF_LOGI(TAG, "NONE mode: demo draws nothing, LVGL (or whatever "
                     "else is running) owns every pixel");
        return;
    }

    /* The one real sprite this build has, loaded through the asset
     * pipeline (docs/architecture/adr-0033-asset-pipeline.md) rather than
     * baked into a header: kf_assets_init() (called from kf_app_init(),
     * before this function runs) has already mounted the pack and built
     * its descriptor table, so this is a lookup, not a load. Copied by
     * value into d.sprite -- kf_sprite is a flat POD, so this copies the
     * pointer/width/height/color-key fields, not the pixel bytes
     * themselves, which stay wherever kf_hal_assets_base() put them. */
    const kf_sprite *test_sprite = kf_assets_get("test_sprite");
    KF_ASSERT(test_sprite != nullptr,
              "asset pack has no sprite named 'test_sprite' -- regenerate "
              "examples/hello_sprite/assets.kfpack with "
              "tools/kf_pack_assets.py --test-sprite");
    d.sprite = *test_sprite;

    /* The sprite's dimensions now come from the pack at runtime rather than
     * a compile-time macro, so -- unlike before -- the compiler can no
     * longer prove KF_DISPLAY_WIDTH/HEIGHT minus them stays non-negative.
     * Assert it once, here, then work in explicit uint32_t ranges below so
     * the subtraction itself stays in a signed type the compiler CAN
     * reason about, with no implicit sign-changing conversion at the
     * kf_rng_below() call boundary (-Wsign-conversion, -Werror). */
    KF_ASSERT(d.sprite.width <= KF_DISPLAY_WIDTH &&
                  d.sprite.height <= KF_DISPLAY_HEIGHT,
              "test_sprite is %ux%u, too big for the %dx%d display",
              d.sprite.width, d.sprite.height, KF_DISPLAY_WIDTH,
              KF_DISPLAY_HEIGHT);
    const uint32_t x_range =
        static_cast<uint32_t>(KF_DISPLAY_WIDTH - d.sprite.width);
    const uint32_t y_range =
        static_cast<uint32_t>(KF_DISPLAY_HEIGHT - d.sprite.height);

    d.mover_count = (mode == KF_DEMO_FULLSCREEN) ? kStressSprites : 1;

    for (int i = 0; i < d.mover_count; ++i) {
        Mover &m = d.movers[i];
        m.x = static_cast<int32_t>(kf_rng_below(x_range)) * kSub;
        m.y = static_cast<int32_t>(kf_rng_below(y_range)) * kSub;
        m.vx = 20 + static_cast<int32_t>(kf_rng_below(24u));
        m.vy = 20 + static_cast<int32_t>(kf_rng_below(24u));
        if (kf_rng_next() & 1u) {
            m.vx = -m.vx;
        }
        if (kf_rng_next() & 1u) {
            m.vy = -m.vy;
        }
    }

    /* Centre the single sprite so the simple demo starts tidily. */
    if (mode == KF_DEMO_SPRITE) {
        d.movers[0].x = static_cast<int32_t>(x_range / 2u) * kSub;
        d.movers[0].y = static_cast<int32_t>(y_range / 2u) * kSub;
    }

    d.background = KF_RGB(24, 26, 38);
    d.needs_full_repaint = true;

    if (mode == KF_DEMO_FULLSCREEN) {
        build_tileset();
        d.scroll_x = 0;
        d.scroll_y = 0;
        KF_LOGI(TAG,
                "FULL-SCREEN mode: %dx%d scrolling tilemap + %d sprites, "
                "every pixel redrawn every frame",
                kTilesAcross, kTilesDown, d.mover_count);
    } else {
        KF_LOGI(TAG, "sprite mode: one sprite, dirty-rectangle repaint");
    }
}

void kf_demo_update(uint32_t held, uint32_t pressed) {
    if (d.mode == KF_DEMO_NONE) {
        return;
    }

    const int32_t nudge = 8;
    Mover &lead = d.movers[0];

    if (held & KF_BTN_LEFT) {
        lead.x -= nudge;
        d.scroll_x -= 24;
    }
    if (held & KF_BTN_RIGHT) {
        lead.x += nudge;
        d.scroll_x += 24;
    }
    if (held & KF_BTN_UP) {
        lead.y -= nudge;
        d.scroll_y -= 24;
    }
    if (held & KF_BTN_DOWN) {
        lead.y += nudge;
        d.scroll_y += 24;
    }

    if (pressed & KF_BTN_A) {
        d.background = static_cast<kf_color>(kf_rng_next() & 0xFFFFu);
        d.needs_full_repaint = true;
    }
    if (pressed & KF_BTN_B) {
        for (int i = 0; i < d.mover_count; ++i) {
            d.movers[i].vx = -d.movers[i].vx;
            d.movers[i].vy = -d.movers[i].vy;
        }
    }

    /* The tilemap drifts on its own, so the full-screen mode is genuinely
     * animating every pixel rather than only when a button is held. */
    if (d.mode == KF_DEMO_FULLSCREEN) {
        d.scroll_x += 14;
        d.scroll_y += 9;
    }

    const int32_t max_x = (KF_DISPLAY_WIDTH - d.sprite.width) * kSub;
    const int32_t max_y = (KF_DISPLAY_HEIGHT - d.sprite.height) * kSub;

    for (int i = 0; i < d.mover_count; ++i) {
        Mover &m = d.movers[i];
        m.x += m.vx;
        m.y += m.vy;
        if (m.x < 0) {
            m.x = 0;
            m.vx = -m.vx;
        }
        if (m.x > max_x) {
            m.x = max_x;
            m.vx = -m.vx;
        }
        if (m.y < 0) {
            m.y = 0;
            m.vy = -m.vy;
        }
        if (m.y > max_y) {
            m.y = max_y;
            m.vy = -m.vy;
        }
    }
}

void kf_demo_draw(void) {
    if (d.mode == KF_DEMO_NONE) {
        return;
    }

    if (d.mode == KF_DEMO_FULLSCREEN) {
        /* No cleverness at all. Repaint the world, then everything on it.
         * The dirty rectangle ends up covering the screen, and the budget
         * report shows exactly what that costs on the device. */
        draw_tilemap();
        for (int i = 0; i < d.mover_count; ++i) {
            const kf_rect r = sprite_rect(d.movers[i]);
            kf_blit(&d.sprite, r.x0, r.y0);
        }
        return;
    }

    const kf_rect now = sprite_rect(d.movers[0]);

    if (d.needs_full_repaint) {
        /* The expensive case: the whole buffer down the wire, about 30ms at
         * 40MHz. The budget report will show it. */
        kf_fill(d.background);
        d.needs_full_repaint = false;
    } else {
        /* Repaint only where the sprite was, then draw it where it is. Two
         * small rectangles instead of a whole screen. */
        kf_fill_rect(d.previous, d.background);
    }

    kf_blit(&d.sprite, now.x0, now.y0);
    d.previous = now;
}

void kf_demo_shutdown(void) {}

void kf_demo_request_full_repaint(void) { d.needs_full_repaint = true; }
