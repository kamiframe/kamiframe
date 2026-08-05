/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_info_screen.h"

#include "../pet/kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/pet.h"

#include <cstdint>

namespace {

constexpr const char *TAG = "pet-info-screen";

struct Screen {
    lv_obj_t *root = nullptr;
    lv_obj_t *stage_label = nullptr;
    lv_obj_t *time_label = nullptr;
    lv_obj_t *branch_label = nullptr;
    bool ready = false;
};
Screen g;

/* "egg"/"baby"/etc, matching kf_pet_screen.cpp's blob_style() captions
 * exactly -- one name per stage, defined once there conceptually even
 * though it is a small, deliberate duplication here (this file has no
 * reason to depend on kf_pet_screen.cpp, the same "small duplication over
 * a shared header" call this project has already made a few times, e.g.
 * kf_pet_session.cpp's total_age_seconds() vs sdl_debug_window.cpp's
 * timeline_tick_seconds()). */
const char *stage_name(kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG:
        return "egg";
    case KF_PET_STAGE_BABY:
        return "baby";
    case KF_PET_STAGE_CHILD:
        return "child";
    case KF_PET_STAGE_TEEN:
        return "teen";
    case KF_PET_STAGE_ADULT:
    default:
        return "adult";
    }
}

/* Formats `seconds` as the single largest two units that make it readable
 * -- "2d 4h", "3h 12m", "5m 09s", "42s" -- rather than a fixed h:m:s
 * layout that would show "0d 0h" for most of an egg's life. No float, no
 * libc string functions: this is simulator-side presentation code (the
 * same allowance kf_pet_screen.cpp's update_row() already uses
 * lv_label_set_text_fmt under), just kept simple because there is no
 * reason not to be. */
void set_duration_label(lv_obj_t *label, uint64_t seconds) {
    /* uint64_t, matching kf_pet_state::stage_elapsed_seconds's own type
     * (kf/pet.h) exactly, rather than narrowing to uint32_t here and
     * inviting a -Wconversion warning (or worse, silent truncation) on a
     * debug-scrubbed pet whose stage_elapsed_seconds a seek deliberately
     * jumped a long way -- see kf_pet_session.h's kf_pet_session_debug_
     * seek() header comment. Everyday values are tiny relative to
     * uint64_t's range regardless. */
    constexpr uint64_t kMinute = 60u;
    constexpr uint64_t kHour = 60u * kMinute;
    constexpr uint64_t kDay = 24u * kHour;

    if (seconds >= kDay) {
        lv_label_set_text_fmt(label, "%u" "d %u" "h",
                               static_cast<unsigned>(seconds / kDay),
                               static_cast<unsigned>((seconds % kDay) / kHour));
    } else if (seconds >= kHour) {
        lv_label_set_text_fmt(label, "%u" "h %u" "m",
                               static_cast<unsigned>(seconds / kHour),
                               static_cast<unsigned>((seconds % kHour) / kMinute));
    } else if (seconds >= kMinute) {
        lv_label_set_text_fmt(label, "%u" "m %02u" "s",
                               static_cast<unsigned>(seconds / kMinute),
                               static_cast<unsigned>(seconds % kMinute));
    } else {
        lv_label_set_text_fmt(label, "%u" "s", static_cast<unsigned>(seconds));
    }
}

} // namespace

void kf_pet_info_screen_init(void) {
    /* lv_obj_create(nullptr) makes a brand-new screen object without
     * touching lv_screen_active() -- kf_pet_screen.cpp's Home screen (or
     * whichever screen happens to be loaded right now) is left exactly as
     * it was. See kf_screen_nav.cpp for when this actually gets shown. */
    g.root = lv_obj_create(nullptr);

    lv_obj_t *title = lv_label_create(g.root);
    lv_label_set_text(title, "Info");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *stage_caption = lv_label_create(g.root);
    lv_label_set_text(stage_caption, "Stage");
    lv_obj_align(stage_caption, LV_ALIGN_TOP_LEFT, 12, 40);
    g.stage_label = lv_label_create(g.root);
    lv_obj_align(g.stage_label, LV_ALIGN_TOP_LEFT, 12, 60);

    lv_obj_t *time_caption = lv_label_create(g.root);
    lv_label_set_text(time_caption, "Time in stage");
    lv_obj_align(time_caption, LV_ALIGN_TOP_LEFT, 12, 96);
    g.time_label = lv_label_create(g.root);
    lv_obj_align(g.time_label, LV_ALIGN_TOP_LEFT, 12, 116);

    /* Only meaningful once a branch has actually been picked (teen or
     * adult) -- update() below leaves this blank before then, the same
     * "no invented names before there is something to name" restraint
     * kf_pet_screen.cpp's blob_style() header comment already documents. */
    g.branch_label = lv_label_create(g.root);
    lv_obj_align(g.branch_label, LV_ALIGN_TOP_LEFT, 12, 152);

    g.ready = true;
    kf_pet_info_screen_update();
}

void kf_pet_info_screen_update(void) {
    KF_ASSERT(g.ready,
              "kf_pet_info_screen_update called before kf_pet_info_screen_init");
    const kf_pet_state *state = kf_pet_session_state();

    lv_label_set_text(g.stage_label, stage_name(state->stage));
    set_duration_label(g.time_label, state->stage_elapsed_seconds);

    /* teen_form/adult_branch are opaque indices (kf/pet.h) shown as plain
     * numbers once they mean anything, exactly kf_pet_screen.cpp's
     * update_blob() convention for the same fields. */
    if (state->stage == KF_PET_STAGE_TEEN) {
        lv_label_set_text_fmt(g.branch_label, "Teen form %u",
                               static_cast<unsigned>(state->teen_form));
    } else if (state->stage == KF_PET_STAGE_ADULT) {
        lv_label_set_text_fmt(g.branch_label, "Adult form %u-%u",
                               static_cast<unsigned>(state->teen_form),
                               static_cast<unsigned>(state->adult_branch));
    } else {
        lv_label_set_text(g.branch_label, "");
    }
}

lv_obj_t *kf_pet_info_screen_root(void) {
    KF_ASSERT(g.ready,
              "kf_pet_info_screen_root called before kf_pet_info_screen_init");
    return g.root;
}
