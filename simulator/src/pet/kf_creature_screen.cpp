/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf_creature_screen.h"

#include "kf_pet_session.h"

#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/creature.h"
#include "kf/pet.h"

#include <cstring>

namespace {

/* The bottom 60 rows are reserved for a stats band a later plan adds (see
 * kf_screen_nav.cpp's own comment on kf_pet_screen.cpp staying unreachable
 * rather than deleted) -- this screen only ever DRAWS INTO y=[0,260):
 * kField is where the creature walks, gets erased, and gets redrawn every
 * frame, and nothing below y=260 ever gets sprite or placeholder content
 * from this file. That is a different claim from "this file never touches
 * those rows at all" -- see kScreen and kf_creature_screen_enter() below for
 * the one place it does, and why. */
constexpr kf_rect kField = {0, 0, 240, 260};

/* The full display, used only by kf_creature_screen_enter()'s entry repaint
 * -- see that function's own comment for why entry needs the whole panel
 * and per-frame drawing above only ever needs kField. */
constexpr kf_rect kScreen = {0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT};

constexpr kf_color kBackground = KF_RGB(232, 240, 216);

/* Stands in for a creature sprite the asset pack does not have yet -- the
 * checked-in default pack (examples/hello_sprite/assets.kfpack) carries
 * exactly one sprite, "test_sprite", so every kf_creature_sprite_name()
 * lookup this screen makes returns nullptr from kf_assets_get() today, on
 * every stage/pose/direction, on both desktop and the device. Drawing
 * nothing in that case would make the screen look BROKEN rather than
 * unfinished, and would make the dirty-rect budget test below pass
 * vacuously (no draw, no dirty rectangle, nothing to measure) -- see
 * run_creature_screen_check()'s own comment. So: visible and obviously
 * wrong beats invisible and indistinguishable from a rendering bug, the
 * same reasoning kf_creature_sprite_name() already applies to the DEAD pose
 * falling back to the sick sprite (kf/creature.h). Hot pink, deliberately
 * nothing any finished creature art would plausibly ship in -- it reads as
 * "placeholder" on sight, the same way a missing-texture checkerboard does
 * in other engines. Disappears the moment the pack carries real creature
 * sprites; nothing else about this file changes when that happens. */
constexpr kf_color kPlaceholderColor = KF_RGB(255, 0, 128);

/* Mess (Task 5). pet->poop_count (kf/pet.h) is a COUNT, 0..KF_PET_MAX_POOPS,
 * with no positions -- Core says so deliberately, because where each one
 * sits on screen is presentation's call, not the simulation's. This is that
 * call: KF_PET_MAX_POOPS fixed slots in a single row, addressed by index
 * alone -- deterministic, so the same count always paints the same pixels,
 * no jitter frame to frame and no RNG (Controller amendment A6: placing
 * mess with the RNG would make a bug report un-reproducible).
 *
 * The row sits inside kField (A3): kPoopSlotWidth * KF_PET_MAX_POOPS ==
 * kField.x1, so all eight slots fit the 240px width with no overlap and
 * none spill outside it, and the row itself (kPoopY0..kPoopY1) finishes
 * well short of y=260, the reserved stats band this file never draws into
 * otherwise -- with a further gap below the row and the band edge, so mess
 * reads as sitting on the floor rather than colliding with the band. */
constexpr int16_t kPoopSize = 12;
constexpr int16_t kPoopY0 = 232;
constexpr int16_t kPoopY1 = kPoopY0 + kPoopSize;
constexpr int16_t kPoopSlotWidth =
    static_cast<int16_t>(kField.x1 / static_cast<int16_t>(KF_PET_MAX_POOPS));

/* Makes the block comment above's "kPoopSlotWidth * KF_PET_MAX_POOPS ==
 * kField.x1" claim true by construction rather than merely asserted in
 * prose: kPoopSlotWidth is computed by integer division just above, which
 * silently floors on a remainder, so a KF_PET_MAX_POOPS that does not
 * evenly divide kField.x1 would otherwise leave the row short of the
 * field's full width -- still safe (never overlapping or spilling), just
 * quietly not what the comment says. This fails the build instead. */
static_assert(kPoopSlotWidth * static_cast<int16_t>(KF_PET_MAX_POOPS) ==
                  kField.x1,
              "kPoopSlotWidth * KF_PET_MAX_POOPS must equal kField.x1 -- "
              "see the block comment above kPoopSize for why");

/* The whole mess row, used only when the poop count actually changes -- see
 * kf_creature_screen_frame()'s mess-drawing comment for why repainting
 * through this one rect, rather than each poop's own rect standing alone,
 * is what keeps that the ONE frame it happens on cheap. */
constexpr kf_rect kMessBand = {0, kPoopY0, kField.x1, kPoopY1};

/* A plain brown, unmistakably neither the field background nor
 * kPlaceholderColor's hot pink -- nothing subtle is being attempted here,
 * the same "obviously placeholder" reasoning kPlaceholderColor's own
 * comment gives, until real mess art exists. */
constexpr kf_color kPoopColor = KF_RGB(92, 64, 51);

/* Where poop number `index` (0-based, < pet->poop_count) sits. Pure
 * function of the index alone -- see the block comment above kPoopSize for
 * why that matters (Controller amendment A6: no RNG). */
kf_rect poop_rect(uint8_t index) {
    const int16_t x0 = static_cast<int16_t>(
        index * kPoopSlotWidth + (kPoopSlotWidth - kPoopSize) / 2);
    return kf_rect{x0, kPoopY0, static_cast<int16_t>(x0 + kPoopSize), kPoopY1};
}

kf_creature g_creature;
kf_rect g_previous = {0, 0, 0, 0};
bool g_up = false;

/* How many poops are actually painted on screen right now -- NOT
 * pet->poop_count, which the mess-drawing code below compares this
 * against every frame precisely so it can tell "the count changed since
 * last frame, repaint" from "nothing to do". -1 is not a value
 * pet->poop_count (uint8_t) can ever hold, so it means "nothing painted
 * yet, or the screen was just wiped" -- the state right after
 * kf_creature_screen_init() and after every kf_creature_screen_enter()
 * (Controller amendment A2: the entry repaint wipes any mess along with
 * everything else, and nothing in the per-frame path would ever notice
 * unless this is reset too). Either way it forces the very next frame to
 * repaint regardless of what pet->poop_count already is. */
int g_drawn_poops = -1;

/* Remembers the outcome of the last sprite resolution (Controller amendment
 * A4: west-first lookup) so a creature with no west art pays one strcmp per
 * frame, not two failed kf_assets_get() lookups. `requested_name` is the
 * name actually asked for FIRST -- the "_w_" name when facing west, the
 * plain per-direction name otherwise -- which is also the only thing that
 * can legitimately change frame to frame (the pet's stage/pose/direction),
 * so comparing against it is exactly "did the thing that could change the
 * answer actually change". kf_assets_get()'s own contract (kf/assets.h)
 * is what makes caching the resolved pointer itself safe: valid for the
 * remainder of the program once it is non-null. */
struct SpriteCache {
    char requested_name[32] = {};
    const kf_sprite *sprite = nullptr;
    bool mirrored = false;
    bool valid = false;
};
SpriteCache g_sprite_cache;

/* Resolves the sprite (and whether to draw it mirrored) for this pet's
 * current pose and the creature's current facing, applying the west-first
 * fallback: ask the pack for real "_w_" art first, and only draw the "_e_"
 * sprite mirrored when the pack does not have any -- see kf/creature.h's
 * kf_creature_direction comment and kf/blit.h's kf_blit_mirrored() for the
 * two halves of why. Non-west directions have no fallback at all: build the
 * name, look it up, done. Falls all the way through to nullptr (handled by
 * the caller via the placeholder colour, above) when nothing resolves.
 *
 * Known test-coverage gap, recorded rather than closed: the checked-in
 * default asset pack has no creature art at all (kPlaceholderColor's own
 * comment above), so every kf_assets_get() call this function makes
 * returns nullptr in every test that runs today -- the `sprite == nullptr`
 * branch always taken, the cache-hit branch above only ever caching
 * "nullptr, not mirrored", and kf_blit()/kf_blit_mirrored()/the east-
 * fallback's `mirrored = true` path all consequently unreached. Closing
 * this needs real creature art in a pack a test mounts, which is being
 * generated by another agent right now; repointing KF_ASSET_PACK or
 * hand-rolling a pack here to force it early would break checksum tests
 * that assume today's asset-less pack. Leave it be until that art lands. */
void resolve_sprite(const kf_pet_state *pet, kf_creature_pose pose,
                     kf_creature_direction dir, const kf_sprite **out_sprite,
                     bool *out_mirrored) {
    char name[32];
    kf_creature_sprite_name(pet, pose, dir, name, sizeof(name));

    if (g_sprite_cache.valid &&
        std::strcmp(name, g_sprite_cache.requested_name) == 0) {
        *out_sprite = g_sprite_cache.sprite;
        *out_mirrored = g_sprite_cache.mirrored;
        return;
    }

    const kf_sprite *sprite = kf_assets_get(name);
    bool mirrored = false;
    if (sprite == nullptr && dir == KF_CREATURE_DIR_W) {
        char east_name[32];
        kf_creature_sprite_name(pet, pose, KF_CREATURE_DIR_E, east_name,
                                sizeof(east_name));
        sprite = kf_assets_get(east_name);
        mirrored = (sprite != nullptr);
    }

    std::strncpy(g_sprite_cache.requested_name, name,
                 sizeof(g_sprite_cache.requested_name) - 1u);
    g_sprite_cache.requested_name[sizeof(g_sprite_cache.requested_name) - 1u] =
        '\0';
    g_sprite_cache.sprite = sprite;
    g_sprite_cache.mirrored = mirrored;
    g_sprite_cache.valid = true;

    *out_sprite = sprite;
    *out_mirrored = mirrored;
}

} // namespace

void kf_creature_screen_init(void) {
    kf_creature_init(&g_creature, kField);
    kf_creature_screen_enter();
    g_up = true;
}

void kf_creature_screen_frame(uint32_t dt_ms) {
    if (!g_up) { return; }
    const kf_pet_state *pet = kf_pet_session_state();

    /* Notice a care action that happened since last frame and start the
     * reaction showing on the body. seen_care_actions/reaction_hold_ms live
     * on the presentation-only kf_creature, not on the pet itself -- see
     * kf/creature.h's own comment on why kf_pet_state::last_reaction being
     * sticky needs a caller-owned countdown on top of it. */
    if (pet->care_actions_taken != g_creature.seen_care_actions) {
        g_creature.seen_care_actions = pet->care_actions_taken;
        g_creature.reaction_hold_ms = 1200u;
    }

    kf_creature_update(&g_creature, kField, dt_ms);

    /* Erase where it was, draw where it is. Two dirty rectangles at most:
     * one when the creature did not move this frame, since the erase below
     * and the draw further down then touch the exact same rectangle and
     * merge into one (see kf/framebuffer.h's own comment on
     * kf_fb_mark_dirty()) -- both marked by kf_fill_rect()/kf_blit()/
     * kf_blit_mirrored() themselves. See run_creature_screen_check() for
     * what pins this budget down. */
    kf_fill_rect(g_previous, kBackground);

    /* Mess (Task 5). Static by construction: this only ever paints on a
     * frame where pet->poop_count actually differs from g_drawn_poops (it
     * grew, shrank, or the screen was just entered -- A2), or where the
     * erase just above happened to punch a hole through a poop that was
     * standing at the creature's old position. A frame where neither is
     * true touches no mess pixels and marks no mess rectangle at all --
     * the whole point being that eight poops redrawn every frame, on top
     * of the creature's own (up to two, per Controller amendment A1),
     * would blow past KF_MAX_DIRTY_RECTS. See run_creature_screen_check()
     * for what pins the steady-state budget down.
     *
     * Runs before the creature is drawn below, so the creature paints over
     * any mess it happens to be standing on -- it is walking ON the floor,
     * not under it. */
    if (pet->poop_count != g_drawn_poops) {
        /* The count changed, or this is the first frame after entry.
         * Repaint the row once: redraw every currently-active slot, having
         * first cleared whatever was there before -- UNLESS g_drawn_poops
         * is -1, in which case there is nothing to clear. -1 only ever
         * means "kf_creature_screen_init() or kf_creature_screen_enter()
         * just ran" (g_drawn_poops's own comment), and both of those
         * already fill the WHOLE panel (or field) to kBackground right
         * before setting it -- kMessBand sits entirely inside that fill,
         * so it is already background pixel for pixel. Clearing it again
         * here would cost a second, disjoint dirty rectangle (on top of
         * the one the entry repaint already spent) and 5,760 bytes for a
         * fill that changes no pixel -- see run_creature_screen_check()'s
         * worst_rects budget, which this exact redundancy used to eat.
         * Any other transition (poop_count genuinely grew or shrank while
         * g_drawn_poops was already a real count) DOES need the clear:
         * a shrink would otherwise leave stale poops from slots that are
         * no longer active. This still costs exactly ONE dirty rectangle
         * no matter how many poops are active -- every poop_rect() is
         * fully inside kMessBand, which the fill just below already
         * marked dirty, and a rectangle that overlaps an already-tracked
         * one merges into it instead of adding a new one (kf/framebuffer.h's
         * kf_fb_mark_dirty() comment). This is the one frame this task's
         * budget does not have to hold at 2 -- it is not part of the
         * steady-state loop the check measures, only the rare frame
         * something about the mess actually changed. */
        if (g_drawn_poops != -1) {
            kf_fill_rect(kMessBand, kBackground);
        }
        for (uint8_t i = 0; i < pet->poop_count; ++i) {
            kf_fill_rect(poop_rect(i), kPoopColor);
        }
        g_drawn_poops = pet->poop_count;
    } else {
        /* Nothing about the mess changed -- but the creature's OLD
         * position (g_previous, just erased above) may have been sitting
         * on top of a poop, and that erase painted background straight
         * over it. Put back only the poops the erase actually touched.
         * Free, not merely cheap: each redrawn poop_rect() overlaps
         * g_previous, which the erase already marked dirty, so it merges
         * into that same rectangle rather than adding a new one --
         * Controller amendment A1: "redraw them; do not build machinery
         * to avoid it." */
        for (uint8_t i = 0; i < pet->poop_count; ++i) {
            const kf_rect r = poop_rect(i);
            if (!kf_rect_is_empty(kf_rect_intersect(g_previous, r))) {
                kf_fill_rect(r, kPoopColor);
            }
        }
    }

    const kf_creature_pose pose =
        kf_creature_pose_for(pet, g_creature.reaction_hold_ms);
    const kf_sprite *sprite = nullptr;
    bool mirrored = false;
    resolve_sprite(pet, pose, g_creature.dir, &sprite, &mirrored);

    const kf_rect now = kf_creature_bounds(&g_creature);
    if (sprite != nullptr) {
        if (mirrored) {
            kf_blit_mirrored(sprite, now.x0, now.y0);
        } else {
            kf_blit(sprite, now.x0, now.y0);
        }
    } else {
        /* No art in the pack for this pet yet -- see kPlaceholderColor's
         * own comment above for why this draws something obviously wrong
         * rather than nothing at all. */
        kf_fill_rect(now, kPlaceholderColor);
    }
    g_previous = now;
}

void kf_creature_screen_enter(void) {
    /* Repaints the WHOLE 240x320 panel, not just kField -- because LVGL's
     * display driver flushes into this same framebuffer at the full panel
     * size (kf_lvgl_display.cpp), any LVGL screen (Info, today) can leave
     * pixels sitting in y=[260,320) that this screen would otherwise never
     * touch again: kf_screen_nav_wants_lvgl() is false once Home is active,
     * so the LVGL pump that would normally repaint Info's own leftovers
     * never runs while Home is showing. One dirty rect either way --
     * kScreen fully contains kField, and overlapping/touching rects merge
     * (kf/framebuffer.h's own comment on kf_fb_mark_dirty()) -- so this
     * costs nothing extra against the RECT-COUNT budget run_creature_
     * screen_check() (simulator/src/headless/headless_main.cpp) polices:
     * one dirty rect, same as if only kField had been touched. It is NOT
     * free in BYTES, though -- 240x320x2 = 153,600, well past that same
     * check's 13,824-byte per-frame limit. Nothing breaks: this function
     * is called once per screen switch, never from inside a frame either
     * check measures. Said plainly so the byte cost is not mistaken for
     * zero just because the rect cost is.
     *
     * This does NOT mean the reserved stats band (kField's own comment,
     * above) becomes this screen's to draw into: kBackground here is
     * blank fill, covering over whatever Info left behind, not content.
     * The band stays visually empty -- background colour, nothing drawn on
     * top of it -- until the stats band that owns it lands. */
    kf_fill_rect(kScreen, kBackground);
    g_previous = kf_creature_bounds(&g_creature);

    /* Mess must be invalidated here too (Controller amendment A2): the
     * fill just above just wiped any poops that were painted on screen,
     * along with everything else, but it does not touch pet->poop_count
     * itself -- Core's number is unaffected, only what is currently
     * PAINTED is. Left alone, g_drawn_poops would still read whatever it
     * held before this entry, so the next kf_creature_screen_frame() would
     * see "count unchanged" and draw nothing, and the mess would silently
     * stay invisible until poop_count happens to change for some other
     * reason. -1 is not a real count, so it forces that next frame to
     * repaint regardless of what pet->poop_count already is -- see
     * g_drawn_poops's own comment. Not drawing the mess itself here: that
     * stays kf_creature_screen_frame()'s job, run once, the very next time
     * it is called. */
    g_drawn_poops = -1;
}
