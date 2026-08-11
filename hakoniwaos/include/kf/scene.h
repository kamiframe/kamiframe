/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The retained scene, and the differ that turns "what the game declared this
 * frame" into "what actually needs to be repainted".
 *
 * WHY RETAINED, NOT IMMEDIATE. If a caller could draw a pixel directly --
 * `draw_sprite(x, y)` every frame -- core would have no way to know what
 * changed, and would either have to mark the whole screen dirty every frame
 * (240*320*2 = 153,600 bytes, ~31ms of SPI transfer against a 33.3ms budget,
 * see kf/budget.h's KF_DISPLAY_SPI_HZ arithmetic) or make the caller manage
 * dirty rectangles by hand -- unacceptable for the audience this platform is
 * for (see CLAUDE.md: "a WordPress or jQuery developer should not have too
 * much trouble"). So a caller instead DECLARES what exists -- position,
 * sprite, text, colour -- and kf_scene_commit() diffs that declaration
 * against what was actually painted last frame, computes the minimal set of
 * dirty rectangles itself, and repaints only those. The caller never sees a
 * rectangle.
 *
 * HANDLES, NOT POINTERS. A kf_scene_id is a plain integer that keeps
 * increasing for the life of the program (never reused, not even across
 * kf_scene_reset()), so a stale handle from before a reset stays an
 * "obviously not found" value forever rather than quietly aliasing whatever
 * object now happens to occupy the same storage slot -- a dangling pointer
 * cannot make that promise, because the freed memory can be handed back out
 * and look valid again.
 *
 * TWO FILE-STATIC ARRAYS, NOTHING ELSE. `hakoniwaos/` stays heap-free
 * (tools/check_no_heap.py) and float-free, on the desktop build exactly as
 * on the device -- there is no emulator, this file runs unmodified on both.
 * The object table and the per-frame dirty-candidate scratch space are both
 * fixed-capacity file-static arrays in hakoniwaos/src/scene.cpp; nothing
 * here allocates.
 *
 * WHAT HAPPENS WHEN THE SCENE IS FULL. kf_scene_add_sprite/_text/_box()
 * return 0 (never a valid id) when all KF_SCENE_MAX_OBJECTS slots are
 * taken, and log once naming the limit. They do not panic and do not
 * silently drop the request into a slot that does not exist: 0 is a value
 * every caller can check, and a Lua binding (Task 3) turns it into a
 * catchable script error instead of a crash.
 *
 * STRINGS ARE COPIED, NEVER HELD BY POINTER. A sprite name or a text string
 * handed to this module is copied into a fixed-size field on the object
 * immediately. Holding a pointer instead would be the easy choice and it
 * would be wrong: a Lua string is garbage-collected, and by the time this
 * object is next drawn -- possibly several frames later -- that pointer can
 * already be dangling.
 *
 * See docs/architecture/adr-0040-retained-scene.md for the coalescing rule,
 * the 31ms arithmetic in full, and what "verified" does and does not mean
 * yet (no scene declared through this API has rendered on real hardware).
 */

#ifndef KF_SCENE_H
#define KF_SCENE_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0 is never a valid object -- it is what a full scene or a not-found lookup
 * returns, and every caller can check it without a second "is this valid"
 * call. Ids increase monotonically for the life of the program; see this
 * header's own "HANDLES, NOT POINTERS" comment above. */
typedef uint16_t kf_scene_id;

/* Room for an order of magnitude more than the current home screen needs
 * (creature, up to four poops, three stat bars, three stat labels, five
 * care-guide labels -- under ten objects today) at ~40 bytes each, a couple
 * of KB of static storage. See kf_scene_add_sprite()'s own comment for what
 * happens past this limit. */
#define KF_SCENE_MAX_OBJECTS 64

/* Working set for the coalescer kf_scene_commit() runs before it marks
 * anything dirty -- see that function's own comment for why the cap exists
 * and what happens when a frame's raw candidate count would exceed it. */
#define KF_SCENE_MAX_DIRTY_CANDIDATES 32

/* Longest sprite name kf_scene_add_sprite()/kf_scene_set_sprite() will
 * store, not counting the NUL -- matches the pack format's own 32-byte name
 * field (hakoniwaos/src/assets.cpp's kNameBytes), so nothing a real pack
 * could name is ever truncated. */
#define KF_SCENE_SPRITE_NAME_MAX 31

/* Longest text string kf_scene_add_text()/kf_scene_set_text() will store,
 * not counting the NUL. 40 is not arbitrary: at KF_FONT_CELL_W (6px) that is
 * exactly one full 240px display row, so nothing a caller could usefully
 * show fits less than this limit already covers. */
#define KF_SCENE_TEXT_MAX 40

/* Discards every declared object and the background, and arranges for the
 * NEXT kf_scene_commit() to repaint the whole screen unconditionally --
 * without this, an object that existed before the reset but has no
 * counterpart in whatever gets declared next would leave its last-painted
 * pixels on screen forever, because nothing would ever again contribute its
 * old bounds as a dirty candidate. Object ids already handed out are not
 * reused (see this header's "HANDLES, NOT POINTERS" comment); a stale id
 * from before the reset stays "not found" rather than aliasing whatever
 * object now happens to reuse its old storage slot. */
void kf_scene_reset(void);

/* Forces the NEXT kf_scene_commit() to repaint the whole screen
 * unconditionally, WITHOUT discarding a single object or its id -- the
 * gap kf_scene_reset() above cannot fill for a screen that declares its
 * objects once and holds their handles for the life of the process rather
 * than redeclaring them on every entry (a Lua script's top-level code,
 * unlike kf_creature_screen_enter()'s own re-declare-from-scratch style).
 * A screen with more than one panel hits this the moment something ELSE
 * has drawn to the framebuffer since this screen was last active -- an
 * LVGL screen, another retained scene -- because this screen's own diff
 * still believes its unchanged objects are exactly what is on the panel,
 * when in fact whatever the other screen last painted is. Call this once,
 * on becoming active again, before the next kf_scene_commit(); everything
 * declared before this call keeps its id, position and every other field
 * exactly as it was. See docs/architecture/adr-0043-lua-home-default.md
 * for the gap this closed (ADR 0042's "Known gap: Home re-entry under
 * KF_HOME_SCREEN=lua") and why a full teardown was the wrong fix for it. */
void kf_scene_force_repaint(void);

/* The scene's base layer -- what shows through where nothing else is drawn.
 * Exactly one of colour or sprite is active; the one set most recently
 * wins. Defaults to KF_BLACK on kf_scene_reset() so a scene with nothing
 * declared yet still has a defined appearance rather than showing whatever
 * pixels happened to be in the framebuffer before.
 *
 * A colour background is repainted only where a dirty rectangle actually
 * falls, exactly like any other object -- kf_fill_rect() clips to the exact
 * rectangle it is given. A SPRITE background cannot be clipped this way
 * (kf_blit_frame() clips only to the screen edge, never to an arbitrary
 * rectangle -- see kf/blit.h), so kf_scene_commit() falls back to
 * redrawing the whole screen on any frame where a sprite background is set
 * and anything at all is dirty. That is a real, deliberate cost, not an
 * oversight: see docs/architecture/adr-0040-retained-scene.md's
 * "kf_clip_push()/pop() follow-up, not built" section. */
void kf_scene_set_background_color(kf_color c);
void kf_scene_set_background_sprite(const char *name);

/* Every add_* function returns 0 (never a valid id) once
 * KF_SCENE_MAX_OBJECTS are already live, and logs once naming the limit --
 * see this header's own "WHAT HAPPENS WHEN THE SCENE IS FULL" comment. A
 * new object starts visible, at layer 0, at position (0, 0), and at the
 * front of paint order among objects sharing that layer (creation order is
 * the tie-break -- see kf_scene_commit()'s own comment on why that has to
 * be stable). */
kf_scene_id kf_scene_add_sprite(const char *name);
kf_scene_id kf_scene_add_text(const char *str);
kf_scene_id kf_scene_add_box(int16_t w, int16_t h, kf_color c);

/* Marks the object for removal: its current on-screen area is erased on the
 * NEXT kf_scene_commit(), after which the slot is free for reuse by a later
 * add_*() call (under a new id -- see "HANDLES, NOT POINTERS" above). Every
 * setter below silently does nothing on an id that is 0, not found, or
 * already removed -- there is no error return to check, by design: a
 * removed handle behaving like a no-op rather than a crash is what makes it
 * safe for a Lua binding (Task 3) to mark its own userdata dead and let
 * stray calls through it land here harmlessly instead of chasing every call
 * site. */
void kf_scene_remove(kf_scene_id id);

/* Every setter below is a no-op on an id that does not currently name a
 * live object, and a no-op when called on the wrong kind of object for the
 * field it sets (e.g. kf_scene_set_frame() on a text object) -- see
 * kf_scene_remove()'s own comment for why that is a deliberate design
 * choice, not an omission. */
void kf_scene_set_pos(kf_scene_id id, int16_t x, int16_t y);
void kf_scene_set_visible(kf_scene_id id, bool visible);
void kf_scene_set_layer(kf_scene_id id, int8_t layer);

/* Sprite objects only. */
void kf_scene_set_sprite(kf_scene_id id, const char *name);
void kf_scene_set_frame(kf_scene_id id, uint16_t frame);
void kf_scene_set_mirrored(kf_scene_id id, bool mirrored);

/* Text objects only. */
void kf_scene_set_text(kf_scene_id id, const char *str);

/* Text objects use both fg and bg (kf_text_draw()'s own two-colour cell,
 * kf/font.h). Box objects use only fg, as the box's single fill colour --
 * bg is ignored for a box, so a caller with one colour to set can pass it
 * as both arguments and get the same result either way. Sprite objects
 * ignore this call entirely; a sprite's colours come from its pack data. */
void kf_scene_set_colors(kf_scene_id id, kf_color fg, kf_color bg);

/* Box objects only. Negative width or height clamps to 0 (an empty,
 * invisible box), never a crash. */
void kf_scene_set_size(kf_scene_id id, int16_t w, int16_t h);

/* The object's current (declared, not yet necessarily painted) on-screen
 * rectangle, or an empty rect {0,0,0,0} for an id that is 0, not found,
 * removed, or currently invisible. Every sprite's bounds is exactly 48x48
 * at its declared position -- the Global Constraint that sprites are 48x48
 * (CLAUDE.md) means this never needs to consult the resolved sprite's own
 * dimensions, which in turn is what keeps this call cheap enough to use
 * from a debug accessor (Task 4) without its own caching. */
kf_rect kf_scene_bounds(kf_scene_id id);

/* Diffs this frame's declarations against what was actually painted last
 * frame, computes the minimal set of dirty rectangles, and repaints
 * exactly that. Call once per frame, after the game has finished declaring
 * -- see docs/architecture/adr-0040-retained-scene.md for why this belongs
 * to the frame loop (Task 3) and not to any individual setter above.
 *
 * A frame in which nothing changed marks zero dirty rectangles and draws
 * zero pixels: this is the whole performance argument for retained mode,
 * made checkable rather than asserted. */
void kf_scene_commit(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_SCENE_H */
