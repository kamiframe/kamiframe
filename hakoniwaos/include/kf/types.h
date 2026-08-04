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

/* An immutable sprite. Pixels are RGB565 and are expected to live in flash
 * or an asset arena, never to be copied. */
typedef struct {
    const kf_color *pixels;
    uint16_t width;
    uint16_t height;
    kf_color color_key; /* pixels equal to this are not drawn */
    bool has_color_key;
} kf_sprite;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_TYPES_H */
