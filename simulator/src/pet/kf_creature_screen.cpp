/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors. */

#include "kf_creature_screen.h"

#include "kf_creature_presenter.h"
#include "kf_home_screen_input.h"
#include "kf_pet_session.h"

#include "kf/app.h"
#include "kf/budget.h"
#include "kf/font.h"
#include "kf/pet.h"
#include "kf/scene.h"

#include <cstring>

namespace {

/* Task 4 of the Lua game-layer plan rebuilt this file on kf/scene.h -- the
 * retained differ Task 2 of that plan added -- and Task 5 (the Lua
 * game-layer plan) split the wander/pose/animation orchestration and the
 * hardware care buttons out of this file entirely, into
 * kf_creature_presenter.h and kf_home_screen_input.h respectively:
 * KF_HOME_SCREEN=lua needs the exact same wander result and the exact same
 * buttons working, and this file is no longer the only thing that reads
 * either. What is LEFT here is purely this SCREEN's own layout (where the
 * poops/stat bars/guide sit) and the sequence of scene declarations that
 * turns "the presenter's current answer" and "the pet's current needs" into
 * what this screen shows -- see kf_creature_presenter.h's own header
 * comment for the wander/pose reasoning this file used to carry directly. */

/* The bottom 60 rows are the reserved stats band (Task 9 of the hardware
 * bring-up plan, the hardware bring-up plan) --
 * the creature/mess/shrine drawing this file declares only ever occupies
 * y=[0,260): kField is where the creature walks and where mess collects.
 * The stat bars and the care-button guide live below it, in the band
 * itself -- see their own declarations further down for the exact rows.
 * Aliases KF_CREATURE_PRESENTER_FIELD (kf_creature_presenter.h) rather than
 * defining its own copy -- see that constant's own comment for why this
 * screen and the Lua screen must never be able to disagree about it. */
constexpr kf_rect kField = KF_CREATURE_PRESENTER_FIELD;

constexpr kf_color kBackground = KF_RGB(232, 240, 216);

/* Mess (Task 5 of the pet-screen plan). pet->poop_count (kf/pet.h) is a
 * COUNT, 0..KF_PET_MAX_POOPS, with no positions -- Core says so
 * deliberately, because where each one sits on screen is presentation's
 * call, not the simulation's.
 *
 * KF_PET_MAX_POOPS individual box objects, one per poop, each at its own
 * fixed slot -- see poop_rect() below -- and toggled visible/invisible by
 * declare_mess() as poop_count moves. kf/scene.h's kf_scene_commit() now
 * opportunistically merges any cluster of candidates that costs little to
 * combine (hakoniwaos/src/scene.cpp's kCheapMergeAreaPx), not only once
 * KF_MAX_DIRTY_RECTS forces it, which is what keeps eight genuinely
 * separate poops affordable: whatever subset changes in one frame is
 * always a CONTIGUOUS run of adjacent slots in this one 12px-tall strip --
 * exactly the shape that coalescer collapses to a single dirty rectangle. */
constexpr int16_t kPoopSize = 12;
constexpr int16_t kPoopY0 = 232;
constexpr int16_t kPoopY1 = static_cast<int16_t>(kPoopY0 + kPoopSize);
constexpr int16_t kPoopSlotWidth =
    static_cast<int16_t>(kField.x1 / static_cast<int16_t>(KF_PET_MAX_POOPS));
static_assert(kPoopSlotWidth * static_cast<int16_t>(KF_PET_MAX_POOPS) ==
                  kField.x1,
              "kPoopSlotWidth * KF_PET_MAX_POOPS must equal kField.x1 -- a "
              "full house of mess should span exactly the field's width, no "
              "more and no less");
constexpr kf_color kPoopColor = KF_RGB(92, 64, 51);

kf_rect poop_rect(int index) {
    const int16_t x0 = static_cast<int16_t>(
        index * kPoopSlotWidth + (kPoopSlotWidth - kPoopSize) / 2);
    return kf_rect{x0, kPoopY0, static_cast<int16_t>(x0 + kPoopSize), kPoopY1};
}

/* The death scene (spec: the core care-loop design spec, "Death without a
 * player holds on the last creature's scene"): a small roadside shrine,
 * centred in the field, for as long as pet->dead stays true. */
constexpr const char *kShrineSpriteName = "shrine_idle_s";
constexpr int16_t kShrineSize = 48;

kf_rect centered_in_field(int16_t width, int16_t height) {
    const int16_t x0 = static_cast<int16_t>(
        kField.x0 + ((kField.x1 - kField.x0) - width) / 2);
    const int16_t y0 = static_cast<int16_t>(
        kField.y0 + ((kField.y1 - kField.y0) - height) / 2);
    return kf_rect{x0, y0, static_cast<int16_t>(x0 + width),
                    static_cast<int16_t>(y0 + height)};
}

bool g_up = false;

/* ------------------------------------------------------------------------
 * Stats band (Task 9, the hardware bring-up plan)
 * -- unchanged by Task 5, see kf_creature_presenter.h/kf_home_screen_
 * input.h for what DID move out of this file.
 * ------------------------------------------------------------------------ */
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

kf_rect stat_bar_row_rect(int index) {
    const int16_t y0 =
        static_cast<int16_t>(kStatsRowsY0 + index * kStatsRowPitch);
    return kf_rect{kStatsBarX0, y0,
                    static_cast<int16_t>(kStatsBarX0 + kStatsBarW),
                    static_cast<int16_t>(y0 + kStatsBarH)};
}

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

kf_scene_id g_creature_id = 0;
kf_scene_id g_shrine_id = 0;
kf_scene_id g_poop_id[KF_PET_MAX_POOPS] = {};
kf_scene_id g_stat_track_id[kStatCount] = {0, 0, 0};
kf_scene_id g_stat_fill_id[kStatCount] = {0, 0, 0};

/* Declares the creature's scene object for this frame from whatever
 * kf_creature_presenter_advance() most recently resolved -- this file no
 * longer computes any of pose, facing, the west-mirror fallback, or the
 * animation cursor itself; see kf_creature_presenter.h. Must be called
 * after a kf_creature_presenter_advance() call this same frame. */
void declare_creature() {
    kf_scene_set_visible(g_creature_id, true);
    kf_scene_set_pos(g_creature_id, kf_creature_presenter_x(),
                      kf_creature_presenter_y());
    kf_scene_set_sprite(g_creature_id, kf_creature_presenter_sprite_name());
    kf_scene_set_mirrored(g_creature_id, kf_creature_presenter_mirrored());
    kf_scene_set_frame(g_creature_id, kf_creature_presenter_anim_frame());
}

void declare_mess(const kf_pet_state *pet) {
    for (uint8_t i = 0; i < KF_PET_MAX_POOPS; ++i) {
        kf_scene_set_visible(g_poop_id[i], i < pet->poop_count);
    }
}

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
    kf_creature_presenter_reset();
    kf_creature_screen_enter();
    g_up = true;
}

void kf_creature_screen_frame(uint32_t dt_ms) {
    if (!g_up) { return; }
    const kf_pet_state *pet = kf_pet_session_state();

    declare_stat_bars(pet);

    kf_scene_set_visible(g_shrine_id, pet->dead);
    kf_scene_set_visible(g_creature_id, !pet->dead);
    if (pet->dead) {
        kf_scene_commit();
        return;
    }

    /* Task 6 (pet-screen plan): read this frame's debounced button edges
     * BEFORE advancing the presenter, so a button-triggered reaction shows
     * up starting this same frame, not one frame late. */
    kf_home_screen_handle_care_buttons(pet, kf_app_buttons_pressed());

    kf_creature_presenter_advance(pet, dt_ms);
    declare_mess(pet);
    declare_creature();

    kf_scene_commit();
}

void kf_creature_screen_enter(void) {
    /* kf_scene_reset() discards every object declared by a previous
     * kf_creature_screen_enter() call and arranges for the next kf_scene_
     * commit() to repaint the whole 240x320 panel unconditionally -- see
     * kf/scene.h's own header comment on why entry needs this (ADR 0017's
     * black-trail bug). */
    kf_scene_reset();
    kf_scene_set_background_color(kBackground);

    for (int i = 0; i < static_cast<int>(KF_PET_MAX_POOPS); ++i) {
        const kf_rect r = poop_rect(i);
        g_poop_id[i] = kf_scene_add_box(kPoopSize, kPoopSize, kPoopColor);
        kf_scene_set_pos(g_poop_id[i], r.x0, r.y0);
    }

    const kf_rect shrine_rect = centered_in_field(kShrineSize, kShrineSize);
    g_shrine_id = kf_scene_add_sprite(kShrineSpriteName);
    kf_scene_set_pos(g_shrine_id, shrine_rect.x0, shrine_rect.y0);
    kf_scene_set_visible(g_shrine_id, false);

    /* Placeholder name -- declare_creature() below sets the real one before
     * this function's own commit, so this initial value is never actually
     * painted. */
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
     * re-entry (Home -> Info -> Home), where the presenter's own wander
     * state has not changed but the scene object it now writes into is a
     * brand new one. See kf_creature_presenter_force_anim_restart()'s own
     * comment. */
    kf_creature_presenter_force_anim_restart();

    const kf_pet_state *pet = kf_pet_session_state();
    declare_mess(pet);
    declare_stat_bars(pet);
    kf_scene_set_visible(g_shrine_id, pet->dead);
    kf_scene_set_visible(g_creature_id, !pet->dead);
    if (!pet->dead) {
        kf_creature_presenter_advance(pet, 0);
        declare_creature();
    }

    kf_scene_commit();
}

void kf_creature_screen_debug_set_direction(kf_creature_direction dir) {
    kf_creature_presenter_debug_set_direction(dir);
}

kf_rect kf_creature_screen_debug_bounds(void) {
    return kf_creature_presenter_debug_bounds();
}

void kf_creature_screen_debug_press(uint32_t buttons) {
    kf_home_screen_handle_care_buttons(kf_pet_session_state(), buttons);
}

uint32_t kf_creature_screen_debug_reaction_hold_ms(void) {
    return kf_creature_presenter_debug_reaction_hold_ms();
}

int16_t kf_creature_screen_debug_egg_bob_offset_y(void) {
    return kf_creature_presenter_debug_egg_bob_offset_y();
}

uint16_t kf_creature_screen_debug_anim_frame(void) {
    return kf_creature_presenter_anim_frame();
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
