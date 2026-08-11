/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_home_screen_input.h. Moved verbatim out of kf_creature_screen.cpp's
 * pre-Task-5 handle_care_buttons().
 */

#include "kf_home_screen_input.h"

#include "kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/types.h"

namespace {

constexpr const char *TAG = "home-screen-input";

/* One variation counter PER ACTION: a shared counter would mean pressing
 * Feed then Play then Feed again skips variation 1 of Feed entirely --
 * each action's own cycle has to be independent of what the others are
 * doing. */
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

} // namespace

void kf_home_screen_handle_care_buttons(const kf_pet_state *pet,
                                        uint32_t pressed) {
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
