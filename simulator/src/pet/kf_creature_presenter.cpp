/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_creature_presenter.h for the "why". This is almost entirely code
 * moved verbatim out of kf_creature_screen.cpp's pre-Task-5 declare_
 * creature()/resolve_sprite_name()/egg_bob_offset_y() -- the extraction is
 * the point, not a rewrite.
 */

#include "kf_creature_presenter.h"

#include "kf/assets.h"

#include <cstring>

namespace {

/* The egg bob: pure integer triangle wave, no trig, no float -- see the
 * pre-Task-5 kf_creature_screen.cpp for the derivation this comment used
 * to carry in full; unchanged by the extraction. */
constexpr uint32_t kEggBobPeriodMs = 3000u;
constexpr int16_t kEggBobAmplitudePx = 2;
constexpr uint32_t kEggBobQuarterMs = kEggBobPeriodMs / 4u;
static_assert(kEggBobQuarterMs * 4u == kEggBobPeriodMs,
              "kEggBobPeriodMs must be a multiple of 4");

int16_t egg_bob_offset_y(uint32_t elapsed_ms) {
    const uint32_t phase = elapsed_ms % kEggBobPeriodMs;
    const uint32_t a = static_cast<uint32_t>(kEggBobAmplitudePx);
    int32_t offset;
    if (phase < kEggBobQuarterMs) {
        offset = static_cast<int32_t>((phase * a) / kEggBobQuarterMs);
    } else if (phase < 2u * kEggBobQuarterMs) {
        offset = static_cast<int32_t>(
            ((2u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    } else if (phase < 3u * kEggBobQuarterMs) {
        offset = -static_cast<int32_t>(
            ((phase - 2u * kEggBobQuarterMs) * a) / kEggBobQuarterMs);
    } else {
        offset = -static_cast<int32_t>(
            ((4u * kEggBobQuarterMs - phase) * a) / kEggBobQuarterMs);
    }
    return static_cast<int16_t>(offset);
}

kf_creature g_creature;
bool g_was_egg = true;
uint32_t g_egg_bob_elapsed_ms = 0u;

/* Resolved output of the last kf_creature_presenter_advance() call. */
int16_t g_x = 0;
int16_t g_y = 0;
char g_sprite_name[32] = {};
bool g_mirrored = false;
uint16_t g_anim_frame = 0u;

/* The sprite NAME actually requested last advance() -- used only to notice
 * a pose/facing change so the animation cursor resets to frame 0 there
 * rather than being left mid-cycle for one visible frame. See kf_creature_
 * presenter.h's own header comment: this is g_creature's concern (what
 * value to hand the animation cursor), not scene state. */
char g_last_requested_name[32] = {};

/* Resolves the sprite name (and whether to draw it mirrored) for `pose`/
 * `dir`, applying the west-first fallback, and resets the animation cursor
 * on a pose/facing change. Writes straight into the module globals above --
 * moved verbatim from kf_creature_screen.cpp's resolve_sprite_name(). */
void resolve_and_declare(const kf_pet_state *pet, kf_creature_pose pose,
                          kf_creature_direction dir) {
    char requested[32];
    kf_creature_sprite_name(pet, pose, dir, requested, sizeof(requested));

    if (std::strcmp(requested, g_last_requested_name) != 0) {
        g_creature.anim.frame = 0u;
        g_creature.anim.accum_ms = 0u;
        std::strncpy(g_last_requested_name, requested,
                     sizeof(g_last_requested_name) - 1u);
        g_last_requested_name[sizeof(g_last_requested_name) - 1u] = '\0';
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

    std::strncpy(g_sprite_name, final_name, sizeof(g_sprite_name) - 1u);
    g_sprite_name[sizeof(g_sprite_name) - 1u] = '\0';
    g_mirrored = mirrored;

    kf_creature_anim_wrap(&g_creature,
                          sprite != nullptr ? sprite->frame_count : 0u);
    g_anim_frame = g_creature.anim.frame;
}

} // namespace

void kf_creature_presenter_reset(void) {
    kf_creature_init(&g_creature, KF_CREATURE_PRESENTER_FIELD);
    g_was_egg = true;
    g_egg_bob_elapsed_ms = 0u;
    g_last_requested_name[0] = '\0';
}

void kf_creature_presenter_advance(const kf_pet_state *pet, uint32_t dt_ms) {
    if (pet->care_actions_taken != g_creature.seen_care_actions) {
        g_creature.seen_care_actions = pet->care_actions_taken;
        g_creature.reaction_hold_ms = 1200u;
    }

    if (pet->stage == KF_PET_STAGE_EGG) {
        if (!g_was_egg) {
            kf_creature_init(&g_creature, KF_CREATURE_PRESENTER_FIELD);
        }
        g_egg_bob_elapsed_ms += dt_ms;
        kf_creature_tick_anim(&g_creature, dt_ms);
    } else {
        kf_creature_update(&g_creature, KF_CREATURE_PRESENTER_FIELD, dt_ms);
    }
    g_was_egg = (pet->stage == KF_PET_STAGE_EGG);

    const kf_creature_pose pose =
        kf_creature_pose_for(pet, g_creature.reaction_hold_ms);
    resolve_and_declare(pet, pose, g_creature.dir);

    kf_rect now = kf_creature_bounds(&g_creature);
    if (pet->stage == KF_PET_STAGE_EGG) {
        now.y0 = static_cast<int16_t>(now.y0 + egg_bob_offset_y(g_egg_bob_elapsed_ms));
    }
    g_x = now.x0;
    g_y = now.y0;
}

void kf_creature_presenter_force_anim_restart(void) {
    g_last_requested_name[0] = '\0';
}

int16_t kf_creature_presenter_x(void) { return g_x; }
int16_t kf_creature_presenter_y(void) { return g_y; }
const char *kf_creature_presenter_sprite_name(void) { return g_sprite_name; }
bool kf_creature_presenter_mirrored(void) { return g_mirrored; }
uint16_t kf_creature_presenter_anim_frame(void) { return g_anim_frame; }

void kf_creature_presenter_debug_set_direction(kf_creature_direction dir) {
    g_creature.dir = dir;
}

uint32_t kf_creature_presenter_debug_reaction_hold_ms(void) {
    return g_creature.reaction_hold_ms;
}

int16_t kf_creature_presenter_debug_egg_bob_offset_y(void) {
    return egg_bob_offset_y(g_egg_bob_elapsed_ms);
}

kf_rect kf_creature_presenter_debug_bounds(void) {
    return kf_creature_bounds(&g_creature);
}
