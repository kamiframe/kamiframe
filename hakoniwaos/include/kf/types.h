/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Shared plain-data types. Valid C: this header sits on the HAL boundary.
 */

#ifndef KF_TYPES_H
#define KF_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A pixel. RGB565, native-endian. */
typedef uint16_t kf_color;

/* Pack 8-bit components into RGB565. Lossy, obviously: you lose 3 bits of
 * red and blue and 2 of green. Sprites should be authored in 565 rather than
 * converted at runtime. */
#define KF_RGB(r, g, b)                                                       \
    ((kf_color)((((uint16_t)(r) & 0xF8u) << 8) |                              \
                (((uint16_t)(g) & 0xFCu) << 3) | (((uint16_t)(b) & 0xF8u) >> 3)))

#define KF_BLACK KF_RGB(0, 0, 0)
#define KF_WHITE KF_RGB(255, 255, 255)

/* Half-open rectangle: x0,y0 inclusive, x1,y1 exclusive.
 * Empty is x1 <= x0 || y1 <= y0. */
typedef struct {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
} kf_rect;

/* Result codes. Deliberately small: this is not an error-handling framework.
 * Anything genuinely unrecoverable calls kf_panic instead of returning. */
typedef enum {
    KF_OK = 0,
    KF_ERR_UNAVAILABLE = 1, /* the backend does not provide this */
    KF_ERR_INVALID = 2,     /* caller passed something nonsensical */
    KF_ERR_EXHAUSTED = 3,   /* out of the relevant fixed resource */
    KF_ERR_IO = 4
} kf_result;

/* Wall-clock time, Unix epoch seconds.
 *
 * Deliberately separate from the monotonic clock. This one can be unset, can
 * jump forward when the user fixes their timezone, and can go backwards when
 * the coin cell dies or the clock is corrected. The pet's offline ageing
 * depends on it, so every consumer must handle all three cases. */
typedef struct {
    int64_t epoch_seconds;
    bool valid; /* false before the RTC has ever been set */
} kf_wall_time;

/* Buttons, as a bitmask. The HAL reports raw levels only. Debounce, repeat,
 * edge detection and chords are core's job, so the device and the simulator
 * feel identical. */
typedef enum {
    KF_BTN_UP = 1u << 0,
    KF_BTN_DOWN = 1u << 1,
    KF_BTN_LEFT = 1u << 2,
    KF_BTN_RIGHT = 1u << 3,
    KF_BTN_A = 1u << 4,
    KF_BTN_B = 1u << 5,
    KF_BTN_MENU = 1u << 6
} kf_button;

#define KF_BUTTON_COUNT 7

/* How a sprite's pixels are stored. A property of the DATA, not a request
 * from the caller -- which is why it lives on the sprite rather than being
 * a flag passed to kf_blit(). See kf/blit.h's own comment for why that
 * distinction is what makes this different from kf_blit_mirrored(). */
typedef enum {
    KF_SPRITE_FORMAT_RGB565 = 0,  /* `pixels` is valid */
    KF_SPRITE_FORMAT_INDEXED8 = 1 /* `indices` + `palette` are valid */
} kf_sprite_format;

/* The palette slot a colour-keyed indexed sprite reserves for "do not draw
 * this pixel". Fixed at 0 by convention rather than stored per sprite: it
 * costs nothing, it makes the blitter's key test a compare against a
 * compile-time constant, and the packer is what guarantees it (see
 * tools/kf_ingest_sprites.py, which forces the magenta key to index 0). */
#define KF_SPRITE_KEY_INDEX 0u

/* An immutable sprite, possibly with more than one frame. Pixels live in
 * flash or a mounted asset pack and are never copied.
 *
 * FRAMES ARE CONTIGUOUS. For an indexed sprite, frame k starts at
 * `indices + k * width * height` -- one directory entry per animation, O(1)
 * frame addressing, no per-frame name lookup. RGB565 sprites are always
 * frame_count == 1; multi-frame RGB565 was never packed and is not worth
 * adding when everything is migrating to indexed anyway. */
typedef struct {
    const kf_color *pixels;  /* RGB565 data; NULL when format is INDEXED8 */
    const uint8_t *indices;  /* 8bpp palette indices; NULL when RGB565 */
    const kf_color *palette; /* palette_count entries; NULL when RGB565 */
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;   /* always >= 1 */
    uint16_t palette_count; /* 0 when RGB565 */
    kf_color color_key;     /* for INDEXED8 this equals palette[0] */
    bool has_color_key;     /* pixels equal to color_key are not drawn */
    uint8_t format;         /* a kf_sprite_format value */
} kf_sprite;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_TYPES_H */
