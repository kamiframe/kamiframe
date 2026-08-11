/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf/scene.h for the "why" of every decision named in this file's
 * comments. This file is the "how".
 */

#include "kf/scene.h"

#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/font.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "scene";

/* Same colour, same reasoning, as
 * simulator/src/pet/kf_creature_screen.cpp's kPlaceholderColor: obviously,
 * unmissably wrong, so a missing sprite is a bug someone notices on the
 * panel, not a blank space nobody investigates. */
constexpr kf_color kPlaceholderColor = KF_RGB(255, 0, 128);

/* Global Constraint (CLAUDE.md): every sprite is 48x48. bounds_of() below
 * relies on this being true for every sprite object regardless of whether
 * its name has resolved yet -- see kf/scene.h's own comment on
 * kf_scene_bounds() for why that keeps this module from ever needing to
 * consult a resolved kf_sprite's own width/height. */
constexpr int16_t kSpriteSize = 48;

/* What a text object paints in until something calls kf_scene_set_colors().
 * kf_scene_add_text() takes no colour arguments, so this IS the default a
 * script sees from `kf.text("HI")` with no `:color()` call -- white on the
 * KF_BLACK that RenderState::bg already defaults to. Applied by
 * kf_scene_add_text() at runtime rather than as RenderState::fg's own
 * initialiser, purely so g_objects stays zero-initialised; both comments
 * carry the detail, and run_scene_check()'s check 5
 * (simulator/src/headless/headless_main.cpp) fails if this stops being what
 * a default text object actually paints. */
constexpr kf_color kDefaultTextFg = KF_WHITE;

constexpr kf_rect kEmptyRect = {0, 0, 0, 0};
constexpr kf_rect kFullScreenRect = {
    0, 0, static_cast<int16_t>(KF_DISPLAY_WIDTH),
    static_cast<int16_t>(KF_DISPLAY_HEIGHT)};

enum class ObjKind : uint8_t { kSprite, kText, kBox };

/* Every field kf_scene_commit() compares to decide whether an object needs
 * repainting, plus everything needed to paint it. One flat shape for all
 * three kinds rather than a union or inheritance: KF_SCENE_MAX_OBJECTS (64)
 * is small enough that the handful of always-unused fields on any one
 * object cost nothing, and a flat struct means comparison and painting
 * never reach into a union by kind -- they just read the fields that
 * matter for THIS object's kind and ignore the rest. */
struct RenderState {
    int16_t x = 0;
    int16_t y = 0;
    /* Starts false on purpose. SceneObject::presented below is never
     * explicitly initialised to "not yet painted" with a separate flag --
     * a brand-new object's presented.visible is false while its
     * declared.visible is set true by whichever add_*() created it (see
     * that function), so the very first kf_scene_commit() sees a
     * visible-false -> visible-true change and treats the object as new,
     * with no extra bookkeeping. */
    bool visible = false;
    int8_t layer = 0;

    char sprite_name[KF_SCENE_SPRITE_NAME_MAX + 1] = {};
    uint16_t frame = 0;
    bool mirrored = false;

    char text[KF_SCENE_TEXT_MAX + 1] = {};
    /* Deliberately zero (KF_BLACK), NOT the white a text object actually
     * defaults to -- kf_scene_add_text() applies that white at runtime, and
     * kDefaultTextFg below is where the reason lives. Every other field in
     * this struct and in SceneObject is already zero-initialised, so this
     * one initialiser decides whether g_objects lands in .bss (zero-filled
     * at boot, RAM only) or .data (baked into flash and copied to RAM at
     * boot, costing 14,336 bytes of flash on top of the RAM). Anything
     * added here later should stay zero for the same reason. */
    kf_color fg = KF_BLACK;
    /* Box objects only ever read `fg`; `bg` exists for text's two-colour
     * cell (kf_text_draw()) and is simply unused on a box object -- see
     * kf_scene_set_colors()'s own comment. */
    kf_color bg = KF_BLACK;

    int16_t w = 0; /* box only */
    int16_t h = 0; /* box only */
};

struct SceneObject {
    bool in_use = false;
    /* True from kf_scene_remove() until the commit that erases this
     * object's last painted area has run; see kf_scene_commit()'s own
     * comment on why removal cannot free the slot immediately. */
    bool removed = false;
    kf_scene_id id = 0;
    ObjKind kind = ObjKind::kSprite;

    RenderState declared;  /* what the game asked for this frame */
    RenderState presented; /* what was actually painted last commit */

    /* Sprite name resolution cache: kf_assets_get() runs again only when
     * `resolved_name` (the name this cache was built for) stops matching
     * `declared.sprite_name` -- a linear scan over the asset directory
     * (hakoniwaos/src/assets.cpp) is fine once per change and wasteful
     * every frame for an object that is not even moving. This is what let
     * Task 4 of the Lua game-layer plan (docs/superpowers/plans/
     * 2026-08-12-lua-game-layer.md) delete simulator/src/pet/kf_creature_
     * screen.cpp's own hand-rolled equivalent (g_sprite_cache) entirely:
     * the scene now does this once, here, for every caller, instead of
     * each caller needing its own copy. Safe to hold onto: kf/assets.h's
     * kf_assets_get() promises the pointer stays valid for the remainder
     * of the program. */
    char resolved_name[KF_SCENE_SPRITE_NAME_MAX + 1] = {};
    const kf_sprite *resolved_sprite = nullptr;
};

SceneObject g_objects[KF_SCENE_MAX_OBJECTS];

/* Ids increase for the life of the program and are never reused, even
 * across kf_scene_reset() -- see kf/scene.h's "HANDLES, NOT POINTERS"
 * comment. Skips 0 on wraparound; KF_SCENE_MAX_OBJECTS (64) live objects at
 * a time means wrapping past 65,535 total creations would need thousands of
 * add/remove cycles, which is a real but distant concern worth a comment,
 * not a wider type this early. */
kf_scene_id g_next_id = 1;

enum class BgKind : uint8_t { kColor, kSprite };

struct Background {
    BgKind kind = BgKind::kColor;
    kf_color color = KF_BLACK;
    char sprite_name[KF_SCENE_SPRITE_NAME_MAX + 1] = {};
};

Background g_bg_declared;
Background g_bg_presented;
char g_bg_resolved_name[KF_SCENE_SPRITE_NAME_MAX + 1] = {};
const kf_sprite *g_bg_resolved_sprite = nullptr;

/* True on the very first commit after startup or kf_scene_reset() -- see
 * kf_scene_reset()'s own comment for why a reset has to force a full
 * repaint rather than only diffing against whatever the new scene
 * declares. */
bool g_force_full_redraw = true;

/* The coalescer's working set -- see kf/scene.h's own comment on
 * KF_SCENE_MAX_DIRTY_CANDIDATES for why this is capped rather than sized to
 * the theoretical worst case (every object changing in the same frame,
 * contributing an old and a new rect each, is 2 * KF_SCENE_MAX_OBJECTS =
 * 128 candidates). */
kf_rect g_candidates[KF_SCENE_MAX_DIRTY_CANDIDATES];
int g_candidate_count = 0;

int16_t clamp_to_i16(int32_t v) {
    if (v < INT16_MIN) {
        return INT16_MIN;
    }
    if (v > INT16_MAX) {
        return INT16_MAX;
    }
    return static_cast<int16_t>(v);
}

SceneObject *find(kf_scene_id id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto &obj : g_objects) {
        /* A removed object is deliberately excluded here, not just
         * skipped by the caller: this is the ONE check every setter and
         * kf_scene_bounds() routes through, so "removed objects behave as
         * not found" holds everywhere at once rather than needing to be
         * remembered at each call site. */
        if (obj.in_use && !obj.removed && obj.id == id) {
            return &obj;
        }
    }
    return nullptr;
}

/* Copies `src` into `dst` (capacity `dst_cap`, including the NUL), silently
 * truncating and logging once if it does not fit. `id == 0` means "the
 * background", which has no per-object id to name in the log line. */
void copy_truncated(char *dst, size_t dst_cap, const char *src,
                     kf_scene_id id, const char *what) {
    if (src == nullptr) {
        src = "";
    }
    const size_t src_len = std::strlen(src);
    std::snprintf(dst, dst_cap, "%s", src);
    if (src_len > dst_cap - 1u) {
        if (id == 0) {
            KF_LOGW(TAG, "background %s '%s' is %zu characters, truncated to %zu",
                    what, src, src_len, dst_cap - 1u);
        } else {
            KF_LOGW(TAG,
                    "scene object %u: %s '%s' is %zu characters, truncated "
                    "to %zu",
                    static_cast<unsigned>(id), what, src, src_len,
                    dst_cap - 1u);
        }
    }
}

kf_scene_id allocate(ObjKind kind) {
    for (auto &obj : g_objects) {
        if (obj.in_use) {
            continue;
        }
        obj = SceneObject{};
        obj.in_use = true;
        obj.kind = kind;
        obj.id = g_next_id++;
        if (g_next_id == 0u) {
            g_next_id = 1u; /* skip 0 -- see g_next_id's own comment */
        }
        obj.declared.visible = true;
        return obj.id;
    }
    KF_LOGE(TAG, "scene is full (%d objects) -- ignoring add",
            KF_SCENE_MAX_OBJECTS);
    return 0;
}

/* An object's on-screen rectangle for the given (declared or presented)
 * state, or an empty rect when it would not be drawn at all -- invisible,
 * or a box with zero or negative size. Sprites are always kSpriteSize
 * regardless of whether their name has resolved to real art (an unresolved
 * sprite still draws a kPlaceholderColor box that size, so the space it
 * occupies on screen -- and therefore what must be marked dirty -- is
 * identical either way). */
kf_rect bounds_of(ObjKind kind, const RenderState &s) {
    if (!s.visible) {
        return kEmptyRect;
    }
    int16_t w = 0;
    int16_t h = 0;
    switch (kind) {
    case ObjKind::kSprite:
        w = kSpriteSize;
        h = kSpriteSize;
        break;
    case ObjKind::kText:
        w = kf_text_width(s.text);
        h = KF_FONT_CELL_H;
        break;
    case ObjKind::kBox:
        w = s.w;
        h = s.h;
        break;
    }
    if (w <= 0 || h <= 0) {
        return kEmptyRect;
    }
    return kf_rect{s.x, s.y,
                   clamp_to_i16(static_cast<int32_t>(s.x) + w),
                   clamp_to_i16(static_cast<int32_t>(s.y) + h)};
}

/* Whether `a` would paint anything differently from `b` -- position,
 * visibility and layer matter for every kind (layer because a stacking
 * order change can matter even when nothing moved: two overlapping objects
 * that swap paint order need repainting even though neither one's own
 * rectangle changed), the rest is kind-specific. */
bool changed(ObjKind kind, const RenderState &a, const RenderState &b) {
    if (a.x != b.x || a.y != b.y || a.visible != b.visible ||
        a.layer != b.layer) {
        return true;
    }
    switch (kind) {
    case ObjKind::kSprite:
        return std::strcmp(a.sprite_name, b.sprite_name) != 0 ||
               a.frame != b.frame || a.mirrored != b.mirrored;
    case ObjKind::kText:
        return std::strcmp(a.text, b.text) != 0 || a.fg != b.fg ||
               a.bg != b.bg;
    case ObjKind::kBox:
        return a.w != b.w || a.h != b.h || a.fg != b.fg;
    }
    return false;
}

/* How much area merging `a` and `b` into one bounding rectangle adds beyond
 * what the two already cover separately. Zero or negative for a pair that
 * already touches or overlaps (kf_fb_mark_dirty()'s own touches_or_overlaps
 * rule, framebuffer.cpp:43, folds those into one rectangle for free
 * already), positive and growing with how far apart two disjoint
 * rectangles are -- so picking the minimum always prefers a free merge
 * over a costly one. int64_t: each area fits comfortably in uint32_t, but
 * the subtraction below can go negative, which uint32_t cannot represent. */
int64_t merge_cost(kf_rect a, kf_rect b) {
    return static_cast<int64_t>(kf_rect_area(kf_rect_union(a, b))) -
           static_cast<int64_t>(kf_rect_area(a)) -
           static_cast<int64_t>(kf_rect_area(b));
}

/* A merge is worth taking on its own merits -- not just because
 * KF_MAX_DIRTY_RECTS forces one -- when it costs under this many pixels
 * (2,048 bytes at RGB565's 2 bytes/pixel) beyond what the two rectangles
 * already cover separately. This is what lets a cluster of small, same-band
 * objects collapse to one dirty rectangle even when nothing about
 * KF_SCENE_MAX_OBJECTS or KF_MAX_DIRTY_RECTS ever forces a merge: the
 * canonical case is kf_creature_screen.cpp's mess, up to eight individually
 * declared poop objects (kept as separate objects, deliberately, so the
 * game keeps them visually discrete) roughly 18px apart in a 12px-tall
 * strip. Two neighbouring 12x12 poops cost 216 extra pixels to merge, and
 * -- because the strip is evenly spaced -- every further neighbour in the
 * same run costs the same ~216px again, not a growing amount, so a whole
 * contiguous run of changed poops collapses to one rectangle a cheap step
 * at a time, regardless of how many candidates the rest of the scene
 * happens to have that frame. 1,024px sits comfortably above that (roughly
 * 4x headroom for slop in exact spacing) and comfortably below what merging
 * two genuinely unrelated regions of the same screen costs: the same mess
 * strip merged with the stats band nine rows below it -- the nearest other
 * thing on the creature screen -- costs over 4,000px, four times this
 * budget, so the two never merge by accident. See
 * docs/architecture/adr-0040-retained-scene.md for the fuller reasoning. */
constexpr int64_t kCheapMergeAreaPx = 1024;

/* Scans every pair in `rects[0..count)` for the cheapest one to merge
 * (merge_cost(), above) WITHOUT merging it -- a caller that wants to decide
 * whether a merge is actually worth taking (kf_scene_commit()'s coalescing
 * loop) can inspect the cost first; a caller that always wants the merge
 * regardless of cost calls merge_cheapest_pair() below instead. Returns
 * INT64_MAX for `count` under 2 (nothing to merge), so a caller comparing
 * this against a threshold does not need its own separate guard for that
 * case. */
int64_t find_cheapest_pair(const kf_rect *rects, int count, int *out_i,
                            int *out_j) {
    *out_i = 0;
    *out_j = 1;
    if (count < 2) {
        return INT64_MAX;
    }
    int64_t best_cost = merge_cost(rects[0], rects[1]);
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            const int64_t cost = merge_cost(rects[i], rects[j]);
            if (cost < best_cost) {
                best_cost = cost;
                *out_i = i;
                *out_j = j;
            }
        }
    }
    return best_cost;
}

/* Merges rects[j] into rects[i] and drops the count by one, moving the
 * last element into the gap left at j -- the actual mutation
 * find_cheapest_pair() above deliberately stops short of, so a caller can
 * inspect the cost before paying for it. */
void merge_pair(kf_rect *rects, int *count, int i, int j) {
    rects[i] = kf_rect_union(rects[i], rects[j]);
    rects[j] = rects[*count - 1];
    (*count)--;
}

/* Removes one rectangle from `rects[0..*count)` by merging the globally
 * cheapest pair (find_cheapest_pair(), above) regardless of what that
 * costs. Used to make room in the capped candidate buffer
 * (KF_SCENE_MAX_DIRTY_CANDIDATES), where the alternative is dropping a
 * candidate outright -- a hard buffer limit, not a frame-budget judgement
 * call, so "merge unconditionally" is the right rule here even though
 * kf_scene_commit()'s own coalescing loop, below, does NOT reuse this: that
 * loop wants to see the cost before deciding a merge is worth it (see
 * kCheapMergeAreaPx above). */
void merge_cheapest_pair(kf_rect *rects, int *count) {
    if (*count < 2) {
        return;
    }
    int i, j;
    find_cheapest_pair(rects, *count, &i, &j);
    merge_pair(rects, count, i, j);
}

void add_candidate(kf_rect r) {
    if (kf_rect_is_empty(r)) {
        return;
    }
    if (g_candidate_count == KF_SCENE_MAX_DIRTY_CANDIDATES) {
        /* Full: KF_SCENE_MAX_DIRTY_CANDIDATES is a hard cap on file-static
         * storage, not a soft target -- make room by merging the cheapest
         * existing pair before this one is appended, rather than growing
         * past it. */
        merge_cheapest_pair(g_candidates, &g_candidate_count);
    }
    g_candidates[g_candidate_count++] = r;
}

/* Resolves `declared_name` through kf_assets_get() only when it differs
 * from `*resolved_name` (the name the cache currently reflects), caching
 * the result either way -- including a failed lookup, so a name that does
 * not exist is not retried every frame either. `id_for_log == 0` means the
 * background, which has no per-object id. */
void resolve_sprite(char *resolved_name, size_t resolved_name_cap,
                     const kf_sprite **resolved_sprite,
                     const char *declared_name, kf_scene_id id_for_log) {
    if (std::strcmp(resolved_name, declared_name) == 0) {
        return;
    }
    std::snprintf(resolved_name, resolved_name_cap, "%s", declared_name);
    *resolved_sprite = kf_assets_get(resolved_name);
    if (*resolved_sprite == nullptr) {
        if (id_for_log == 0) {
            KF_LOGE(TAG,
                    "background sprite '%s' not found in the mounted pack "
                    "-- drawing a placeholder",
                    resolved_name);
        } else {
            KF_LOGE(TAG,
                    "scene object %u: sprite '%s' not found in the mounted "
                    "pack -- drawing a placeholder",
                    static_cast<unsigned>(id_for_log), resolved_name);
        }
    }
}

/* Fills `order[0..*out_count)` with the indices into g_objects of every
 * live, non-removed, currently-visible object, sorted by layer ascending
 * and, within a layer, by id ascending -- id order IS creation order
 * (kf_scene_add_*() hands out strictly increasing ids), so this is exactly
 * "layer ascending, ties by creation order" (kf/scene.h's own paint-order
 * comment). A plain insertion sort: KF_SCENE_MAX_OBJECTS is 64, and this
 * runs once per commit, not once per pixel. */
void sorted_paint_order(int *order, int *out_count) {
    int n = 0;
    for (int i = 0; i < KF_SCENE_MAX_OBJECTS; ++i) {
        if (g_objects[i].in_use && !g_objects[i].removed &&
            g_objects[i].declared.visible) {
            order[n++] = i;
        }
    }
    for (int i = 1; i < n; ++i) {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0) {
            const SceneObject &a = g_objects[order[j]];
            const SceneObject &b = g_objects[key];
            const bool a_after_b =
                (a.declared.layer > b.declared.layer) ||
                (a.declared.layer == b.declared.layer && a.id > b.id);
            if (!a_after_b) {
                break;
            }
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }
    *out_count = n;
}

/* Paints the background within `rect`. A colour background clips exactly
 * to `rect` for free (kf_fill_rect() takes the rect as an argument); a
 * sprite background does not (kf_blit_frame() clips only to the screen
 * edge) -- see kf/scene.h's kf_scene_set_background_sprite() comment for
 * why kf_scene_commit() only ever calls this with `rect` equal to the
 * whole screen when a sprite background is in play. */
void paint_background(kf_rect rect) {
    if (g_bg_declared.kind == BgKind::kColor) {
        kf_fill_rect(rect, g_bg_declared.color);
        return;
    }
    if (g_bg_resolved_sprite != nullptr) {
        kf_blit_frame(g_bg_resolved_sprite, 0, 0, 0);
    } else {
        kf_fill_rect(rect, kPlaceholderColor);
    }
}

void paint_object(const SceneObject &obj) {
    const kf_rect bounds = bounds_of(obj.kind, obj.declared);
    switch (obj.kind) {
    case ObjKind::kSprite:
        if (obj.resolved_sprite != nullptr) {
            if (obj.declared.mirrored) {
                kf_blit_frame_mirrored(obj.resolved_sprite, obj.declared.x,
                                       obj.declared.y, obj.declared.frame);
            } else {
                kf_blit_frame(obj.resolved_sprite, obj.declared.x,
                              obj.declared.y, obj.declared.frame);
            }
        } else {
            kf_fill_rect(bounds, kPlaceholderColor);
        }
        break;
    case ObjKind::kText:
        kf_text_draw(obj.declared.x, obj.declared.y, obj.declared.text,
                     obj.declared.fg, obj.declared.bg);
        break;
    case ObjKind::kBox:
        kf_fill_rect(bounds, obj.declared.fg);
        break;
    }
}

} // namespace

void kf_scene_reset(void) {
    for (auto &obj : g_objects) {
        obj = SceneObject{};
    }
    g_bg_declared = Background{};
    g_bg_presented = Background{};
    g_candidate_count = 0;
    /* Forces the next commit to repaint the whole screen -- see this
     * flag's own file-static comment for why a reset cannot rely on the
     * ordinary diff-against-presented path. */
    g_force_full_redraw = true;
    /* g_next_id is deliberately untouched -- see its own comment. */
}

void kf_scene_force_repaint(void) {
    /* Literally the same flag kf_scene_reset() sets -- the only difference
     * from a reset is everything this function deliberately does NOT do:
     * no g_objects wipe, no g_bg_declared/g_bg_presented reset, no
     * g_candidate_count reset (there is nothing pending to discard; the
     * next commit rebuilds its candidate list from scratch regardless, per
     * the g_force_full_redraw branch already in kf_scene_commit()). One
     * object, one flag -- see kf/scene.h's own comment for the case this
     * exists for. */
    g_force_full_redraw = true;
}

void kf_scene_set_background_color(kf_color c) {
    g_bg_declared.kind = BgKind::kColor;
    g_bg_declared.color = c;
}

void kf_scene_set_background_sprite(const char *name) {
    g_bg_declared.kind = BgKind::kSprite;
    copy_truncated(g_bg_declared.sprite_name, sizeof(g_bg_declared.sprite_name),
                   name, 0, "sprite name");
}

kf_scene_id kf_scene_add_sprite(const char *name) {
    const kf_scene_id id = allocate(ObjKind::kSprite);
    if (id == 0) {
        return 0;
    }
    SceneObject *obj = find(id);
    copy_truncated(obj->declared.sprite_name,
                   sizeof(obj->declared.sprite_name), name, id, "sprite name");
    return id;
}

kf_scene_id kf_scene_add_text(const char *str) {
    const kf_scene_id id = allocate(ObjKind::kText);
    if (id == 0) {
        return 0;
    }
    SceneObject *obj = find(id);
    copy_truncated(obj->declared.text, sizeof(obj->declared.text), str, id,
                   "text");
    /* Applied here rather than as RenderState::fg's initialiser so that
     * g_objects stays all-zero at load time and therefore lands in .bss --
     * see RenderState::fg's own comment for the 14,336 bytes of flash that
     * buys. This is the only assignment that makes the difference: a box
     * gets its fg from kf_scene_add_box()'s required argument, and a sprite
     * ignores fg entirely (kf_scene_set_colors()'s contract, kf/scene.h),
     * so text is the one kind with no other source for it.
     *
     * `presented.fg` is deliberately left at zero: it is only ever read by
     * changed(), and a brand-new object is already reported as changed by
     * the presented.visible false -> declared.visible true transition
     * allocate() sets up (see RenderState::visible's own comment). The one
     * case where fg could decide changed() by itself -- an object created
     * and hidden before its first commit, with empty text -- produces two
     * empty rectangles, which add_candidate() drops. */
    obj->declared.fg = kDefaultTextFg;
    return id;
}

kf_scene_id kf_scene_add_box(int16_t w, int16_t h, kf_color c) {
    const kf_scene_id id = allocate(ObjKind::kBox);
    if (id == 0) {
        return 0;
    }
    SceneObject *obj = find(id);
    obj->declared.w = w < 0 ? static_cast<int16_t>(0) : w;
    obj->declared.h = h < 0 ? static_cast<int16_t>(0) : h;
    obj->declared.fg = c;
    return id;
}

void kf_scene_remove(kf_scene_id id) {
    SceneObject *obj = find(id);
    if (obj == nullptr) {
        return;
    }
    /* Not freed immediately -- kf_scene_commit() still needs
     * `obj->presented` to know what area to erase. `find()` already
     * excludes removed objects, so every setter (including a second
     * kf_scene_remove()) becomes a safe no-op on this id from here on. */
    obj->removed = true;
}

void kf_scene_set_pos(kf_scene_id id, int16_t x, int16_t y) {
    SceneObject *obj = find(id);
    if (obj == nullptr) {
        return;
    }
    obj->declared.x = x;
    obj->declared.y = y;
}

void kf_scene_set_visible(kf_scene_id id, bool visible) {
    SceneObject *obj = find(id);
    if (obj == nullptr) {
        return;
    }
    obj->declared.visible = visible;
}

void kf_scene_set_layer(kf_scene_id id, int8_t layer) {
    SceneObject *obj = find(id);
    if (obj == nullptr) {
        return;
    }
    obj->declared.layer = layer;
}

void kf_scene_set_sprite(kf_scene_id id, const char *name) {
    SceneObject *obj = find(id);
    if (obj == nullptr || obj->kind != ObjKind::kSprite) {
        return;
    }
    copy_truncated(obj->declared.sprite_name,
                   sizeof(obj->declared.sprite_name), name, id, "sprite name");
}

void kf_scene_set_frame(kf_scene_id id, uint16_t frame) {
    SceneObject *obj = find(id);
    if (obj == nullptr || obj->kind != ObjKind::kSprite) {
        return;
    }
    obj->declared.frame = frame;
}

void kf_scene_set_mirrored(kf_scene_id id, bool mirrored) {
    SceneObject *obj = find(id);
    if (obj == nullptr || obj->kind != ObjKind::kSprite) {
        return;
    }
    obj->declared.mirrored = mirrored;
}

void kf_scene_set_text(kf_scene_id id, const char *str) {
    SceneObject *obj = find(id);
    if (obj == nullptr || obj->kind != ObjKind::kText) {
        return;
    }
    copy_truncated(obj->declared.text, sizeof(obj->declared.text), str, id,
                   "text");
}

void kf_scene_set_colors(kf_scene_id id, kf_color fg, kf_color bg) {
    SceneObject *obj = find(id);
    if (obj == nullptr) {
        return;
    }
    if (obj->kind == ObjKind::kText) {
        obj->declared.fg = fg;
        obj->declared.bg = bg;
    } else if (obj->kind == ObjKind::kBox) {
        obj->declared.fg = fg; /* bg ignored -- see kf/scene.h's comment */
    }
    /* Sprite objects ignore this call entirely: their colours come from
     * the pack, not the scene. */
}

void kf_scene_set_size(kf_scene_id id, int16_t w, int16_t h) {
    SceneObject *obj = find(id);
    if (obj == nullptr || obj->kind != ObjKind::kBox) {
        return;
    }
    obj->declared.w = w < 0 ? static_cast<int16_t>(0) : w;
    obj->declared.h = h < 0 ? static_cast<int16_t>(0) : h;
}

kf_rect kf_scene_bounds(kf_scene_id id) {
    const SceneObject *obj = find(id);
    if (obj == nullptr) {
        return kEmptyRect;
    }
    return bounds_of(obj->kind, obj->declared);
}

int kf_scene_live_object_count(void) {
    int n = 0;
    for (const auto &obj : g_objects) {
        if (obj.in_use && !obj.removed) {
            ++n;
        }
    }
    return n;
}

void kf_scene_commit(void) {
    /* ---- Resolve sprite names that changed since the last commit. Runs
     * for every live sprite object regardless of whether it turns out to
     * be dirty this frame: the check itself is a cheap strcmp against the
     * cached name for anything that has not changed, and doing it here
     * once, up front, means the paint pass below never has to special-case
     * "have I resolved this object yet". ---- */
    for (auto &obj : g_objects) {
        if (!obj.in_use || obj.removed || obj.kind != ObjKind::kSprite) {
            continue;
        }
        resolve_sprite(obj.resolved_name, sizeof(obj.resolved_name),
                       &obj.resolved_sprite, obj.declared.sprite_name, obj.id);
    }
    if (g_bg_declared.kind == BgKind::kSprite) {
        resolve_sprite(g_bg_resolved_name, sizeof(g_bg_resolved_name),
                       &g_bg_resolved_sprite, g_bg_declared.sprite_name, 0);
    }

    /* ---- Collect dirty candidates: for a forced full redraw, one
     * full-screen candidate and nothing else -- every object gets repainted
     * regardless of whether it individually changed, so there is nothing
     * to gain by diffing first. Otherwise, one full-screen candidate if the
     * background changed, and an old-rect/new-rect pair for every object
     * whose declared state differs from what was last presented. ---- */
    g_candidate_count = 0;

    if (g_force_full_redraw) {
        add_candidate(kFullScreenRect);
    } else {
        const bool bg_changed =
            g_bg_declared.kind != g_bg_presented.kind ||
            (g_bg_declared.kind == BgKind::kColor &&
             g_bg_declared.color != g_bg_presented.color) ||
            (g_bg_declared.kind == BgKind::kSprite &&
             std::strcmp(g_bg_declared.sprite_name,
                         g_bg_presented.sprite_name) != 0);
        if (bg_changed) {
            add_candidate(kFullScreenRect);
        }

        for (auto &obj : g_objects) {
            if (!obj.in_use) {
                continue;
            }
            if (obj.removed) {
                /* Only the old position needs erasing -- nothing new gets
                 * drawn for a removed object. */
                add_candidate(bounds_of(obj.kind, obj.presented));
                continue;
            }
            if (changed(obj.kind, obj.declared, obj.presented)) {
                add_candidate(bounds_of(obj.kind, obj.presented));
                add_candidate(bounds_of(obj.kind, obj.declared));
            }
        }
    }

    /* ---- Coalesce. Two different reasons to merge two candidates into
     * one, and this loop keeps taking the globally cheapest available merge
     * (find_cheapest_pair()) as long as EITHER reason applies:
     *
     *   1. MANDATORY. Past KF_MAX_DIRTY_RECTS, kf/framebuffer.h's
     *      kf_fb_mark_dirty() would collapse to one screen-sized box itself
     *      (framebuffer.cpp) -- correct, but exactly the ~31ms-of-transfer
     *      cost this whole module exists to avoid (kf/scene.h's "WHY
     *      RETAINED, NOT IMMEDIATE" comment). Doing the reduction here
     *      first, with full knowledge of every candidate at once, is what
     *      lets the coalescer do better than that fallback.
     *
     *   2. OPPORTUNISTIC. Even comfortably under the cap, if the cheapest
     *      available merge costs under kCheapMergeAreaPx, take it anyway.
     *      Without this, a merge only ever happened once the hard cap
     *      forced it -- fine for a handful of objects scattered across the
     *      whole panel, but wrong for a cluster of small same-band objects
     *      (kCheapMergeAreaPx's own comment walks through the case this
     *      exists for) that stays comfortably under KF_MAX_DIRTY_RECTS on
     *      candidate COUNT alone while still handing the framebuffer a
     *      needlessly long list of tiny, separately-transferred rectangles
     *      a human would call "one obvious patch". This closes exactly the
     *      gap that led Task 4 of the Lua game-layer plan to give up eight
     *      discrete mess poops for one growing box instead of fixing the
     *      coalescer: the box is gone (kf_creature_screen.cpp), and this is
     *      why it no longer needs to exist. ---- */
    for (;;) {
        if (g_candidate_count < 2) {
            break;
        }
        int i = 0;
        int j = 1;
        const int64_t cost =
            find_cheapest_pair(g_candidates, g_candidate_count, &i, &j);
        const bool over_cap = g_candidate_count > KF_MAX_DIRTY_RECTS;
        const bool cheap_enough = cost <= kCheapMergeAreaPx;
        if (!over_cap && !cheap_enough) {
            break;
        }
        merge_pair(g_candidates, &g_candidate_count, i, j);
    }

    /* A sprite background cannot be clipped to a sub-rectangle (see
     * paint_background()'s own comment) -- once anything at all needs
     * repainting, fall back to exactly one full-screen rectangle rather
     * than risk the background repaint issued for one final rectangle
     * overwriting pixels a DIFFERENT final rectangle already finished
     * painting earlier in this same commit. */
    if (g_candidate_count > 0 && g_bg_declared.kind == BgKind::kSprite) {
        g_candidates[0] = kFullScreenRect;
        g_candidate_count = 1;
    }

    if (g_candidate_count == 0) {
        /* Nothing changed: zero dirty rectangles, zero pixels drawn. The
         * headline correctness property (kf/scene.h's kf_scene_commit()
         * comment) and the headline performance win, made checkable rather
         * than asserted. Removed objects are still freed below even on
         * this path -- a removal with nothing else dirty this frame still
         * needs its slot back. */
        g_force_full_redraw = false;
        for (auto &obj : g_objects) {
            if (obj.in_use && obj.removed) {
                obj = SceneObject{};
            }
        }
        return;
    }

    /* ---- Mark every final rectangle dirty BEFORE painting anything.
     * Marking first means every draw call issued below -- each of which
     * marks its own (tighter) rectangle dirty as a side effect of drawing,
     * kf/blit.h's own contract -- lands inside a rectangle the framebuffer
     * already knows about and merges into, rather than adding new slots of
     * its own. Without this ordering, redrawing (say) 12 independently
     * moving objects one at a time could hand the framebuffer 24 separate,
     * mostly non-touching rectangles before ITS OWN merge logic ever got a
     * chance to combine any of them -- past KF_MAX_DIRTY_RECTS, and the
     * framebuffer's fallback is the exact full-screen collapse this
     * function's own coalescing pass, above, exists to avoid. ---- */
    for (int i = 0; i < g_candidate_count; ++i) {
        kf_fb_mark_dirty(g_candidates[i]);
    }

    int order[KF_SCENE_MAX_OBJECTS];
    int order_count = 0;
    sorted_paint_order(order, &order_count);

    /* For each final rectangle: background first (the erase step -- it is
     * what makes a moved object's old position show the background again
     * rather than a smear), then every visible object whose bounds
     * intersect this rectangle, in layer order. An object whose bounds
     * only partially intersects (rather than sit fully inside) a
     * rectangle still gets its FULL bounds redrawn -- kf_blit_frame()
     * cannot clip to less than the whole sprite -- which is real,
     * documented overdraw (docs/architecture/adr-0040-retained-scene.md)
     * but not a correctness problem: the extra pixels this writes are
     * either inside this same rectangle (nothing to worry about) or
     * outside it but IDENTICAL to what was already there (an unchanged
     * object redrawn in place), never something that needed to reach the
     * panel but was not marked dirty. */
    for (int i = 0; i < g_candidate_count; ++i) {
        const kf_rect rect = g_candidates[i];
        paint_background(rect);
        for (int k = 0; k < order_count; ++k) {
            const SceneObject &obj = g_objects[order[k]];
            const kf_rect bounds = bounds_of(obj.kind, obj.declared);
            if (kf_rect_is_empty(kf_rect_intersect(bounds, rect))) {
                continue;
            }
            paint_object(obj);
        }
    }

    /* ---- Declared becomes presented, and removed objects are finally
     * freed -- their erase has now actually happened, above. ---- */
    g_bg_presented = g_bg_declared;
    for (auto &obj : g_objects) {
        if (!obj.in_use) {
            continue;
        }
        if (obj.removed) {
            obj = SceneObject{};
            continue;
        }
        obj.presented = obj.declared;
    }

    g_force_full_redraw = false;
}
