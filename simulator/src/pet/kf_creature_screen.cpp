/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf_creature_screen.h"

#include "kf_pet_session.h"

#include "kf/app.h"
#include "kf/assets.h"
#include "kf/budget.h"
#include "kf/creature.h"
#include "kf/font.h"
#include "kf/hal/log.h"
#include "kf/pet.h"
#include "kf/scene.h"

#include <cstring>

namespace {

constexpr const char *TAG = "creature-screen";

/* Task 4 of the Lua game-layer plan (docs/superpowers/plans/
 * 2026-08-12-lua-game-layer.md) rebuilt this file on kf/scene.h -- the
 * retained differ Task 2 of that plan added. Before this task, every draw in
 * here went straight into the framebuffer through kf/blit.h, and this file
 * hand-tracked what it had last painted (g_previous, g_drawn_poops,
 * g_drawn_stat_px, a sprite-resolution cache) purely so it could work out
 * what needed erasing and redrawing each frame. All of that bookkeeping is
 * gone: this file now DECLARES what should exist -- kf_scene_set_pos(),
 * kf_scene_set_size(), kf_scene_set_sprite() and friends -- once per frame,
 * unconditionally, with the freshly computed value every time, and
 * kf_scene_commit() (called once, at the end of both kf_creature_screen_
 * enter() and kf_creature_screen_frame()) works out what actually changed
 * and repaints exactly that. See kf/scene.h's own "WHY RETAINED, NOT
 * IMMEDIATE" comment for the full reasoning this file now leans on instead
 * of re-deriving.
 *
 * What did NOT move into the scene: kf_creature (g_creature below) is
 * Core-adjacent simulation state -- where the creature is walking to, its
 * facing, its animation clock -- not a description of what is currently
 * painted, so it stays exactly as it was. The scene only ever sees the
 * RESULT of that state each frame (a position, a sprite name, a frame
 * index), never the wander logic that produced it.
 */

/* The bottom 60 rows are the reserved stats band (Task 9 of the hardware
 * bring-up plan, docs/superpowers/plans/2026-08-11-hardware-bringup.md) --
 * the creature/mess/shrine drawing this file declares only ever occupies
 * y=[0,260): kField is where the creature walks and where mess collects.
 * The stat bars and the care-button guide live below it, in the band
 * itself -- see their own declarations further down for the exact rows. */
constexpr kf_rect kField = {0, 0, 240, 260};

constexpr kf_color kBackground = KF_RGB(232, 240, 216);

/* Mess (Task 5). pet->poop_count (kf/pet.h) is a COUNT, 0..KF_PET_MAX_POOPS,
 * with no positions -- Core says so deliberately, because where each one
 * sits on screen is presentation's call, not the simulation's.
 *
 * Before this task, mess was KF_PET_MAX_POOPS individual squares in a row,
 * hand-erased and redrawn together through one shared kMessBand rectangle so
 * that changing the count only ever cost ONE dirty rectangle regardless of
 * how many slots moved. A retained scene object can only be one rectangle
 * of one solid colour -- see kf/scene.h's "What an object is" -- so
 * KF_PET_MAX_POOPS independent objects, each toggled visible/invisible on
 * its own, would cost up to KF_PET_MAX_POOPS separate dirty rectangles on a
 * frame where the count swings a lot (measured: it blows run_creature_
 * screen_budget_combination_check()'s worst_rects<=5u bound). So mess is
 * ONE box object here, growing and shrinking from the left edge of the
 * field with the count -- a solid patch rather than KF_PET_MAX_POOPS
 * discrete squares. Visually different from before (no gaps between
 * individual poops), but nothing in this codebase's test suite polices the
 * exact shape, only that mess is visible, sized roughly to the count, and
 * costs exactly one dirty rectangle when the count changes -- see
 * declare_mess() below for the one place this trade-off lives. */
constexpr int16_t kPoopSize = 12;
constexpr int16_t kPoopY0 = 232;
constexpr int16_t kPoopSlotWidth =
    static_cast<int16_t>(kField.x1 / static_cast<int16_t>(KF_PET_MAX_POOPS));
static_assert(kPoopSlotWidth * static_cast<int16_t>(KF_PET_MAX_POOPS) ==
                  kField.x1,
              "kPoopSlotWidth * KF_PET_MAX_POOPS must equal kField.x1 -- a "
              "full house of mess should span exactly the field's width, no "
              "more and no less");
constexpr kf_color kPoopColor = KF_RGB(92, 64, 51);

/* The egg bob: "bob in one place like it's wobbling/squishing every so
 * often" (the project owner's own words). Pure positioning, an offset
 * applied to where the egg's sprite is DECLARED each frame -- see its use
 * in declare_creature() below. Integer triangle wave, not a lookup table or
 * a sine call: hakoniwaos/ stays free of floating point and of trig, and a
 * triangle wave is exact integer arithmetic that needs neither.
 * kEggBobPeriodMs is how long one full cycle takes; kEggBobAmplitudePx is
 * how far off centre it swings at the extremes.
 *
 * Deliberately NOT applied inside kf_creature_update() (hakoniwaos/src/
 * creature.cpp): that function only knows how to move an (x,y) toward a
 * target, and the egg's bob is not movement, it is a per-frame draw offset
 * on top of a position that never actually changes while the pet is an
 * egg. Keeping it here, next to where it's declared, means Core never
 * needs to know the egg wobbles. */
constexpr uint32_t kEggBobPeriodMs = 3000u;
constexpr int16_t kEggBobAmplitudePx = 2;

constexpr uint32_t kEggBobQuarterMs = kEggBobPeriodMs / 4u;
static_assert(kEggBobQuarterMs * 4u == kEggBobPeriodMs,
              "kEggBobPeriodMs must be a multiple of 4 -- see the block "
              "comment above egg_bob_offset_y() for why");

/* The offset (in whole pixels, positive means drawn lower on screen) for
 * an egg that has been idle for `elapsed_ms`. A plain integer triangle
 * wave, zero-phase-aligned: exactly 0 at elapsed_ms == 0 and at every whole
 * multiple of kEggBobPeriodMs after that -- rising to +kEggBobAmplitudePx a
 * quarter-period in, back through 0 at the half-period mark, down to
 * -kEggBobAmplitudePx at three-quarters, and back to 0 to close the cycle.
 * A fresh egg (g_egg_bob_elapsed_ms starts at 0u) is declared at offset 0
 * on its very first frame, exactly where kf_creature_bounds() already says
 * it is -- see kf_creature_screen_debug_bounds()'s own comment on why that
 * accessor deliberately never includes this offset. */
int16_t egg_bob_offset_y(uint32_t elapsed_ms) {
    const uint32_t phase = elapsed_ms % kEggBobPeriodMs;
    const uint32_t a = static_cast<uint32_t>(kEggBobAmplitudePx);
    int32_t offset;
    if (phase < kEggBobQuarterMs) {
        /* 0 -> +amplitude */
        offset = static_cast<int32_t>((phase * a) / kEggBobQuarterMs);
    } else if (phase < 2u * kEggBobQuarterMs) {
        /* +amplitude -> 0 */
        offset = static_cast<int32_t>(
            ((2u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    } else if (phase < 3u * kEggBobQuarterMs) {
        /* 0 -> -amplitude */
        offset = -static_cast<int32_t>(
            ((phase - 2u * kEggBobQuarterMs) * a) / kEggBobQuarterMs);
    } else {
        /* -amplitude -> 0 */
        offset = -static_cast<int32_t>(
            ((4u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    }
    return static_cast<int16_t>(offset);
}

uint32_t g_egg_bob_elapsed_ms = 0u;

/* The death scene (spec: docs/superpowers/specs/2026-08-09-core-care-loop-
 * design.md, "Death without a player holds on the last creature's scene"):
 * a small roadside shrine, centred in the field, for as long as pet->dead
 * stays true -- see kf_creature_screen_frame()'s dead branch below, which
 * toggles g_shrine_id's visibility directly rather than through a
 * dedicated declare function (there is nothing else about the shrine to
 * declare: its sprite name and position are fixed at creation, in
 * kf_creature_screen_enter()). */
constexpr const char *kShrineSpriteName = "shrine_idle_s";

/* The shrine's footprint. A scene sprite object is always exactly 48x48
 * regardless of whether its name resolves (kf/scene.h's Global Constraint
 * comment), so this is also what the placeholder box the scene draws in
 * shrine_idle_s's absence covers -- one constant serves both cases, unlike
 * the pre-scene version of this file which had to keep centered_in_field()
 * generic against a real sprite's own reported width/height. */
constexpr int16_t kShrineSize = 48;

/* Centres a `width`x`height` box inside kField, floor-dividing any odd
 * remainder. Used for the shrine's fixed position. */
kf_rect centered_in_field(int16_t width, int16_t height) {
    const int16_t x0 = static_cast<int16_t>(
        kField.x0 + ((kField.x1 - kField.x0) - width) / 2);
    const int16_t y0 = static_cast<int16_t>(
        kField.y0 + ((kField.y1 - kField.y0) - height) / 2);
    return kf_rect{x0, y0, static_cast<int16_t>(x0 + width),
                    static_cast<int16_t>(y0 + height)};
}

/* The presentation-only creature: position, wander target, facing, dwell,
 * animation cursor and reaction hold. Genuinely not part of "what is
 * currently painted" -- kf/creature.h's own comment on kf_creature explains
 * why this lives outside Core and is not saved -- so it is not scene state
 * and does not move into kf/scene.h's object table. Every frame, this
 * file's job is to turn whatever this struct currently says into scene
 * declarations; kf_scene_commit() decides what that costs to actually
 * paint. */
kf_creature g_creature;
bool g_up = false;

/* Whether the pet was an egg as of the LAST kf_creature_screen_frame() call
 * that actually looked. See kf_creature_screen_frame()'s own comment on the
 * egg gate for the re-centring this drives. Unrelated to the scene --
 * purely about which branch of g_creature's own update this file takes. */
bool g_was_egg = true;

/* One variation counter PER ACTION -- see handle_care_buttons()'s own header
 * for why a shared counter would be wrong. Unrelated to drawing. */
uint8_t g_feed_variation = 0u;
uint8_t g_play_variation = 0u;
uint8_t g_rest_variation = 0u;
uint8_t g_bath_variation = 0u;

const char *reaction_name(uint8_t reaction) {
    switch (reaction) {
    case KF_PET_REACTION_LIKED:
        return "liked";
    case KF_PET_REACTION_DISLIKED:
        return "disliked";
    case KF_PET_REACTION_NEUTRAL:
    default:
        return "neutral";
    }
}

/* Task 6: hardware button input for the five care actions. See kf/pet.h's
 * kf_pet_care_action and this task's own report for why this exists
 * alongside (not instead of) the Lua binding. Unaffected by the scene
 * rebuild -- this only ever talks to kf_pet_session_*, never to drawing. */
void handle_care_buttons(const kf_pet_state *pet, uint32_t pressed) {
    if (pressed & KF_BTN_A) {
        const uint8_t variation = g_feed_variation;
        g_feed_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_feed(variation);
        KF_LOGI(TAG, "feed variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_UP) {
        const uint8_t variation = g_play_variation;
        g_play_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_play(variation);
        KF_LOGI(TAG, "play variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_DOWN) {
        const uint8_t variation = g_rest_variation;
        g_rest_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_rest(variation);
        KF_LOGI(TAG, "rest variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_LEFT) {
        const uint8_t variation = g_bath_variation;
        g_bath_variation = static_cast<uint8_t>(
            (variation + 1u) % KF_PET_CARE_VARIATION_COUNT);
        kf_pet_session_bath(variation);
        KF_LOGI(TAG, "bath variation=%u reaction=%s", variation,
                reaction_name(pet->last_reaction));
    }
    if (pressed & KF_BTN_RIGHT) {
        kf_pet_session_flush();
        KF_LOGI(TAG, "flush");
    }
}

/* ------------------------------------------------------------------------
 * Stats band (Task 9, docs/superpowers/plans/2026-08-11-hardware-bringup.md)
 *
 * Three bars, one per need kf_pet_state carries (kf/pet.h): hunger_mp,
 * happiness_mp, energy_mp, each 0..KF_PET_MILLIPERCENT_MAX millipercent.
 * Before this task, redrawing a bar only on the frame its QUANTISED pixel
 * width actually changed was this file's own job (update_stat_bar()'s
 * comparison against g_drawn_stat_px). That comparison is gone: this file
 * now calls kf_scene_set_size() with the freshly quantised width every
 * single frame, unconditionally, and kf_scene_commit() is the one that
 * notices most of those calls did not actually change anything and skips
 * the repaint -- exactly the redundant-comparison-elimination retained mode
 * exists for.
 *
 * Each bar is TWO scene objects, not one: a "track" box (the full-width,
 * fixed-colour background) and a "fill" box on top of it (the filled
 * portion, whose width is what actually changes). A scene box can only be
 * one solid colour, so drawing "X% filled, in fill colour, rest in track
 * colour" needs two objects layered -- track at layer 0 (painted first),
 * fill at layer 1 (painted second, on top). The track's own declared state
 * never changes after it is created, so it never itself contributes a
 * dirty candidate; it only ever gets swept along, repainted for free,
 * whenever the fill on top of it does. */
enum { kStatHunger = 0, kStatHappiness = 1, kStatEnergy = 2, kStatCount = 3 };

constexpr const char *kStatLabels[kStatCount] = {"HUNGER", "HAPPY", "ENERGY"};

constexpr kf_color kStatFillColors[kStatCount] = {
    KF_RGB(214, 118, 40), /* hunger: orange */
    KF_RGB(224, 196, 32), /* happiness: yellow */
    KF_RGB(60, 140, 210), /* energy: blue */
};
constexpr kf_color kStatTrackColor = KF_RGB(190, 190, 190);

constexpr int16_t kStatsRowsY0 = 262; /* 2px below kField's own y1 (260) */
constexpr int16_t kStatsRowPitch = static_cast<int16_t>(KF_FONT_CELL_H + 1);
constexpr int16_t kStatsLabelX0 = 2;

constexpr size_t const_strlen(const char *s) {
    return (*s == '\0') ? 0u : 1u + const_strlen(s + 1);
}

constexpr int16_t kStatsLabelZoneChars = 6;
constexpr int16_t kStatsLabelZoneW =
    static_cast<int16_t>(kStatsLabelZoneChars * KF_FONT_CELL_W);
static_assert(const_strlen(kStatLabels[kStatHunger]) <=
                      static_cast<size_t>(kStatsLabelZoneChars) &&
                  const_strlen(kStatLabels[kStatHappiness]) <=
                      static_cast<size_t>(kStatsLabelZoneChars) &&
                  const_strlen(kStatLabels[kStatEnergy]) <=
                      static_cast<size_t>(kStatsLabelZoneChars),
              "kStatsLabelZoneChars must be >= every label's own length");

constexpr int16_t kStatsBarX0 =
    static_cast<int16_t>(kStatsLabelX0 + kStatsLabelZoneW + 4);
constexpr int16_t kStatsBarW = 190;
constexpr int16_t kStatsBarH = KF_FONT_CELL_H;
static_assert(kStatsBarX0 + kStatsBarW <= kField.x1,
              "a stat bar would spill past the field's own right edge "
              "(240px) -- shrink kStatsBarW or kStatsLabelZoneChars");

constexpr int16_t kStatsBandBottomOfBars = static_cast<int16_t>(
    kStatsRowsY0 + (kStatCount - 1) * kStatsRowPitch + kStatsBarH);
static_assert(kStatsBandBottomOfBars < KF_DISPLAY_HEIGHT,
              "the three stat bar rows do not fit above the band's own "
              "bottom edge (320) -- see kStatsRowPitch/kStatsRowsY0");

/* Top-left of stat bar `index`'s row, in framebuffer space -- shared by the
 * track/fill/label positioning below and by stat_bar_filled_px(). */
kf_rect stat_bar_row_rect(int index) {
    const int16_t y0 =
        static_cast<int16_t>(kStatsRowsY0 + index * kStatsRowPitch);
    return kf_rect{kStatsBarX0, y0,
                    static_cast<int16_t>(kStatsBarX0 + kStatsBarW),
                    static_cast<int16_t>(y0 + kStatsBarH)};
}

/* How many of a bar's kStatsBarW pixels should be painted "filled" for a
 * need reading `mp`. Plain integer division, floors. Widening to uint32_t
 * before multiplying: 100000 * 190 is ~1.9e7, comfortably inside uint32_t,
 * but 100000 alone already exceeds int16_t's range. */
int16_t stat_bar_filled_px(kf_pet_millipercent mp) {
    const uint32_t px = (static_cast<uint32_t>(mp) *
                          static_cast<uint32_t>(kStatsBarW)) /
                         KF_PET_MILLIPERCENT_MAX;
    return static_cast<int16_t>(px);
}

constexpr const char *kGuideLabels[5] = {
    "1:FEED", "2:PLAY", "3:REST", "4:BATH", "5:FLUSH",
};

constexpr int16_t kGuideSlotWidth =
    static_cast<int16_t>(kField.x1 / 5);
static_assert(kGuideSlotWidth * 5 == kField.x1,
              "kGuideSlotWidth * 5 must equal kField.x1 -- see the block "
              "comment above kGuideLabels for why");

constexpr int16_t kGuideTextY = static_cast<int16_t>(
    kStatsBandBottomOfBars +
    (KF_DISPLAY_HEIGHT - kStatsBandBottomOfBars - KF_FONT_CELL_H) / 2);
static_assert(kGuideTextY >= kStatsBandBottomOfBars,
              "the guide's text row must not overlap the stat bars above it");
static_assert(kGuideTextY + KF_FONT_CELL_H <= KF_DISPLAY_HEIGHT,
              "the guide's text row must not run past the panel's own "
              "bottom edge");
/* ------------------------------------------------------------------------ */

/* Every scene object this screen owns, re-declared from scratch on every
 * kf_creature_screen_enter() call (kf_scene_reset() invalidates every id
 * previously handed out -- see kf/scene.h's "HANDLES, NOT POINTERS"
 * comment). None of these are meaningful before the first enter() call. */
kf_scene_id g_creature_id = 0;
kf_scene_id g_shrine_id = 0;
kf_scene_id g_mess_id = 0;
kf_scene_id g_stat_track_id[kStatCount] = {0, 0, 0};
kf_scene_id g_stat_fill_id[kStatCount] = {0, 0, 0};

/* The sprite NAME actually requested last time declare_creature() ran --
 * the "_w_" name when facing west, the plain per-direction name otherwise
 * (mirroring resolve_sprite_name()'s own west-first fallback, below). Used
 * for exactly one thing: noticing when the pet's pose or facing changes so
 * the animation cursor can be reset to frame 0 there, rather than left to
 * kf_creature_anim_wrap()'s own clamp -- which is correct but would let a
 * cursor from a longer animation sit mid-cycle for one visible frame before
 * snapping back into range. This is NOT dirty-rect bookkeeping (the scene
 * owns that entirely, via its own per-object resolved-sprite cache in
 * hakoniwaos/src/scene.cpp) -- it exists purely to decide what value to
 * hand the animation cursor, which is g_creature's concern, not the
 * scene's. */
char g_last_requested_sprite_name[32] = {};

/* Resolves the sprite name (and whether to draw it mirrored) for this pet's
 * current pose and the creature's current facing, applying the west-first
 * fallback: ask the pack for real "_w_" art first, and only draw the "_e_"
 * sprite mirrored when the pack has none -- see kf/creature.h's
 * kf_creature_direction comment. Recomputed fresh every frame rather than
 * cached: kf_assets_get() is a cheap linear scan over a pack of at most a
 * few dozen entries (kf/assets.h's own comment), and the scene already
 * caches its OWN resolution of whatever name this settles on, so there is
 * nothing left here worth caching a second time.
 *
 * Deliberately does not touch the scene itself -- this only figures out
 * WHAT to declare; declare_creature() below is the one place that actually
 * calls kf_scene_set_*() for the creature object, so every scene mutation
 * this screen makes stays in one place.
 *
 * `out_name` receives the name actually handed to the scene (the "_e_" name
 * when mirroring, the plain per-direction name otherwise); `out_sprite`
 * receives the resolved pointer (or null), so the caller can read its
 * frame_count for kf_creature_anim_wrap() without a second lookup;
 * `out_mirrored` receives whether to draw it mirrored. */
void resolve_sprite_name(const kf_pet_state *pet, kf_creature_pose pose,
                          kf_creature_direction dir, char *out_name,
                          size_t out_name_len, const kf_sprite **out_sprite,
                          bool *out_mirrored) {
    char requested[32];
    kf_creature_sprite_name(pet, pose, dir, requested, sizeof(requested));

    if (std::strcmp(requested, g_last_requested_sprite_name) != 0) {
        /* The requested name changed since the last frame we looked --
         * start this pose's animation at frame 0 rather than wherever the
         * previous pose's cursor happened to be. */
        g_creature.anim.frame = 0u;
        g_creature.anim.accum_ms = 0u;
        std::strncpy(g_last_requested_sprite_name, requested,
                     sizeof(g_last_requested_sprite_name) - 1u);
        g_last_requested_sprite_name[sizeof(g_last_requested_sprite_name) -
                                      1u] = '\0';
    }

    const kf_sprite *sprite = kf_assets_get(requested);
    bool mirrored = false;
    const char *final_name = requested;
    char east[32];
    if (sprite == nullptr && dir == KF_CREATURE_DIR_W) {
        kf_creature_sprite_name(pet, pose, KF_CREATURE_DIR_E, east,
                                sizeof(east));
        sprite = kf_assets_get(east);
        if (sprite != nullptr) {
            mirrored = true;
            final_name = east;
        }
    }

    std::strncpy(out_name, final_name, out_name_len - 1u);
    out_name[out_name_len - 1u] = '\0';
    *out_sprite = sprite;
    *out_mirrored = mirrored;
}

/* Declares the creature's scene object for this frame: sprite, mirroring,
 * animation frame and position (bobbed while the pet is an egg -- see
 * egg_bob_offset_y() above). Called both from kf_creature_screen_enter()
 * (so the very first commit already shows the right pose, not a blank
 * frame) and from kf_creature_screen_frame()'s alive branch. */
void declare_creature(const kf_pet_state *pet) {
    const kf_creature_pose pose =
        kf_creature_pose_for(pet, g_creature.reaction_hold_ms);

    char name[32];
    const kf_sprite *sprite = nullptr;
    bool mirrored = false;
    resolve_sprite_name(pet, pose, g_creature.dir, name, sizeof(name),
                        &sprite, &mirrored);
    kf_creature_anim_wrap(&g_creature,
                          sprite != nullptr ? sprite->frame_count : 0u);

    kf_rect now = kf_creature_bounds(&g_creature);
    if (pet->stage == KF_PET_STAGE_EGG) {
        const int16_t offset = egg_bob_offset_y(g_egg_bob_elapsed_ms);
        now.y0 = static_cast<int16_t>(now.y0 + offset);
    }

    kf_scene_set_visible(g_creature_id, true);
    kf_scene_set_pos(g_creature_id, now.x0, now.y0);
    kf_scene_set_sprite(g_creature_id, name);
    kf_scene_set_mirrored(g_creature_id, mirrored);
    kf_scene_set_frame(g_creature_id, g_creature.anim.frame);
}

/* Declares the mess object's width for the pet's current poop_count -- see
 * this file's own kPoopSize/kPoopSlotWidth block comment above for why one
 * growing box, not KF_PET_MAX_POOPS discrete ones. Position never changes
 * after creation, so only the size setter is needed here. */
void declare_mess(const kf_pet_state *pet) {
    const int16_t width =
        static_cast<int16_t>(pet->poop_count * kPoopSlotWidth);
    kf_scene_set_size(g_mess_id, width, kPoopSize);
}

/* Refreshes all three fill boxes for the pet's CURRENT needs, unconditionally
 * -- see this file's own stats-band block comment for why there is no
 * change check here any more. */
void declare_stat_bars(const kf_pet_state *pet) {
    const kf_pet_millipercent mp[kStatCount] = {
        pet->hunger_mp, pet->happiness_mp, pet->energy_mp};
    for (int i = 0; i < kStatCount; ++i) {
        const int16_t filled = stat_bar_filled_px(mp[i]);
        kf_scene_set_size(g_stat_fill_id[i], filled, kStatsBarH);
    }
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

    /* Task 9: refresh the stats band before anything else this frame does,
     * including before the death branch just below -- a dead pet's last
     * known needs are still worth showing (the shrine scene freezes
     * everything else about the display the same way). declare_stat_bars()
     * costs nothing on a frame where no need's quantised width actually
     * moved -- kf_scene_commit() (called at the end of this function) is
     * what notices that, not this call site. */
    declare_stat_bars(pet);

    /* The death scene (spec: "Death without a player holds on the last
     * creature's scene") is NOT a creature pose -- no care buttons (Core
     * already no-ops every kf_pet_feed()/_play()/_rest()/_bath()/kf_pet_
     * flush() call once state->dead, see hakoniwaos/src/pet.cpp, so this is
     * a courtesy skip rather than a correctness requirement), no wander, no
     * mess update, and no call to kf_creature_pose_for()/declare_creature()
     * at all -- a shrine is scenery, not something the creature struck a
     * pose as.
     *
     * Unlike before this task, there is no separate "just died" versus
     * "already dead" branch, and no "just revived" branch either: this
     * simply declares the shrine visible and the creature hidden every
     * single frame the pet is dead, and the shrine hidden / creature shown
     * every frame it is not. kf_scene_commit() is what turns a CHANGE in
     * those declarations -- pet->dead flipping either way since the last
     * frame -- into the right dirty rectangles; a frame where dead has not
     * changed since the last one costs nothing further, because neither
     * declaration differs from what is already presented. This is what
     * used to need g_drawn_dead (an explicit "have I already painted the
     * shrine for this death" flag, and a separate revive check to catch
     * pet->dead going back to false without a screen re-entry) -- the
     * differ now makes that bookkeeping unnecessary: it already knows
     * whether the shrine's or the creature's visibility changed, which is
     * the whole question. */
    kf_scene_set_visible(g_shrine_id, pet->dead);
    kf_scene_set_visible(g_creature_id, !pet->dead);
    if (pet->dead) {
        kf_scene_commit();
        return;
    }

    /* Task 6: read this frame's debounced button edges the same way
     * kf_screen_nav.cpp reads MENU/B -- kf_app_buttons_pressed() directly,
     * not LVGL's keypad indev. Must run BEFORE the care_actions_taken check
     * just below, in the same call, so a button-triggered action is
     * noticed the instant it lands rather than one frame late. */
    handle_care_buttons(pet, kf_app_buttons_pressed());

    /* Notice a care action that happened since last frame and start the
     * reaction showing on the body. */
    if (pet->care_actions_taken != g_creature.seen_care_actions) {
        g_creature.seen_care_actions = pet->care_actions_taken;
        g_creature.reaction_hold_ms = 1200u;
    }

    /* The egg does not wander -- see kf_creature_screen.h's own header
     * comment for the full reasoning, unchanged by this task: this gate,
     * the re-centre on a jump back into Egg (g_was_egg), and the reasons
     * for both live entirely in g_creature's own update, not in anything
     * this task touched. */
    if (pet->stage == KF_PET_STAGE_EGG) {
        if (!g_was_egg) {
            kf_creature_init(&g_creature, kField);
        }
        g_egg_bob_elapsed_ms += dt_ms;
        kf_creature_tick_anim(&g_creature, dt_ms);
    } else {
        kf_creature_update(&g_creature, kField, dt_ms);
    }
    g_was_egg = (pet->stage == KF_PET_STAGE_EGG);

    declare_mess(pet);
    declare_creature(pet);

    kf_scene_commit();
}

void kf_creature_screen_enter(void) {
    /* kf_scene_reset() discards every object declared by a previous
     * kf_creature_screen_enter() call (this screen's own objects, or -- on
     * the very first call, from kf_creature_screen_init() -- nothing at
     * all) and arranges for the next kf_scene_commit() to repaint the whole
     * 240x320 panel unconditionally, regardless of what this function goes
     * on to declare. That full-panel repaint is exactly what this screen
     * needs on every entry: whatever screen was active before it (Info,
     * today) can leave real pixels behind in rows this screen would
     * otherwise never touch again -- see kf_creature_screen.h's own header
     * comment on kf_creature_screen_enter() for the full history (ADR
     * 0017's black-trail bug) this guards against. */
    kf_scene_reset();
    kf_scene_set_background_color(kBackground);

    g_mess_id = kf_scene_add_box(0, kPoopSize, kPoopColor);
    kf_scene_set_pos(g_mess_id, 0, kPoopY0);

    const kf_rect shrine_rect = centered_in_field(kShrineSize, kShrineSize);
    g_shrine_id = kf_scene_add_sprite(kShrineSpriteName);
    kf_scene_set_pos(g_shrine_id, shrine_rect.x0, shrine_rect.y0);
    kf_scene_set_visible(g_shrine_id, false);

    /* Placeholder name -- declare_creature() below sets the real one
     * before this function's own commit, so this initial value is never
     * actually painted. */
    g_creature_id = kf_scene_add_sprite("");
    kf_scene_set_layer(g_creature_id, 1); /* paints over the mess */

    for (int i = 0; i < kStatCount; ++i) {
        const kf_rect row = stat_bar_row_rect(i);
        g_stat_track_id[i] =
            kf_scene_add_box(kStatsBarW, kStatsBarH, kStatTrackColor);
        kf_scene_set_pos(g_stat_track_id[i], row.x0, row.y0);

        g_stat_fill_id[i] = kf_scene_add_box(0, kStatsBarH,
                                              kStatFillColors[i]);
        kf_scene_set_pos(g_stat_fill_id[i], row.x0, row.y0);
        kf_scene_set_layer(g_stat_fill_id[i], 1); /* paints over the track */

        const kf_scene_id label = kf_scene_add_text(kStatLabels[i]);
        kf_scene_set_pos(label, kStatsLabelX0, row.y0);
        kf_scene_set_colors(label, KF_BLACK, kBackground);
    }

    /* The on-screen button guide -- drawn once, here, and never again: see
     * handle_care_buttons()'s own header comment on why this is an input
     * affordance rather than the product's real care UI. Text objects with
     * no setter ever called on them again after this cost nothing further
     * against the per-frame dirty-rect budget, the same "painted once,
     * never redeclared" guarantee kf_creature_screen_frame() relies on for
     * the stat labels above. */
    for (int i = 0; i < 5; ++i) {
        const char *label = kGuideLabels[i];
        const int16_t text_w = kf_text_width(label);
        const int16_t slot_x0 = static_cast<int16_t>(i * kGuideSlotWidth);
        const int16_t x = static_cast<int16_t>(
            slot_x0 + (kGuideSlotWidth - text_w) / 2);
        const kf_scene_id guide = kf_scene_add_text(label);
        kf_scene_set_pos(guide, x, kGuideTextY);
        kf_scene_set_colors(guide, KF_BLACK, kBackground);
    }

    /* Forces the very next declare_creature() call to treat this as a
     * fresh pose, resetting the animation cursor -- matters after a real
     * re-entry (Home -> Info -> Home), where g_creature's pose has not
     * changed but the scene object it now writes into is a brand new one
     * with no prior declared sprite name of its own. */
    g_last_requested_sprite_name[0] = '\0';

    const kf_pet_state *pet = kf_pet_session_state();
    declare_mess(pet);
    declare_stat_bars(pet);
    kf_scene_set_visible(g_shrine_id, pet->dead);
    kf_scene_set_visible(g_creature_id, !pet->dead);
    if (!pet->dead) {
        declare_creature(pet);
    }

    kf_scene_commit();
}

void kf_creature_screen_debug_set_direction(kf_creature_direction dir) {
    g_creature.dir = dir;
}

kf_rect kf_creature_screen_debug_bounds(void) {
    return kf_creature_bounds(&g_creature);
}

void kf_creature_screen_debug_press(uint32_t buttons) {
    handle_care_buttons(kf_pet_session_state(), buttons);
}

uint32_t kf_creature_screen_debug_reaction_hold_ms(void) {
    return g_creature.reaction_hold_ms;
}

int16_t kf_creature_screen_debug_egg_bob_offset_y(void) {
    return egg_bob_offset_y(g_egg_bob_elapsed_ms);
}

uint16_t kf_creature_screen_debug_anim_frame(void) {
    return g_creature.anim.frame;
}

kf_rect kf_creature_screen_debug_stat_bar_bounds(int index) {
    if (index < 0 || index >= kStatCount) { return kf_rect{0, 0, 0, 0}; }
    return kf_scene_bounds(g_stat_track_id[index]);
}

int16_t kf_creature_screen_debug_stat_bar_filled_px(int index) {
    if (index < 0 || index >= kStatCount) { return -1; }
    const kf_rect r = kf_scene_bounds(g_stat_fill_id[index]);
    return static_cast<int16_t>(r.x1 - r.x0);
}
