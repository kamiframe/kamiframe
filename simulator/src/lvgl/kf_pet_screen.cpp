/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_screen.h"

#include "../pet/kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/pet.h"

#include <lvgl.h>

#include <cstdint>

namespace {

constexpr const char *TAG = "pet-screen";

struct Row {
    lv_obj_t *bar = nullptr;
    lv_obj_t *value_label = nullptr;
};

struct Screen {
    Row hunger;
    Row happiness;
    Row energy;
    bool ready = false;
};
Screen g;

/* LV_THEME_SIMPLE (kf_lvgl_display.cpp) is exactly what its name says --
 * it does not style LV_STATE_FOCUSED at all, so with the theme alone a
 * keypad-focused button looks identical to an unfocused one. The old proof
 * screen (kf_lvgl_proof_screen.cpp) never exposed this: its group had
 * exactly one member, so "the focused widget" was never ambiguous even
 * though it was never visibly marked either. Three buttons is the first
 * screen where that actually matters -- there is no way to tell which one
 * A/ENTER is about to activate without one. A style applied directly to
 * LV_STATE_FOCUSED, independent of the theme, is the standard LVGL fix; a
 * static lv_style_t is a widget-tree-lifetime constant, not something that
 * needs to be part of `Screen` or re-initialised per screen instance. */
lv_style_t g_focus_style;

void init_focus_style(void) {
    lv_style_init(&g_focus_style);
    lv_style_set_bg_color(&g_focus_style, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_border_color(&g_focus_style, lv_palette_darken(LV_PALETTE_BLUE, 2));
    lv_style_set_border_width(&g_focus_style, 2);
}

/* kf_pet_millipercent is 0..100000 (see kf/pet.h); LVGL's lv_bar wants an
 * integer range, and tenths of a percent (0..1000) gives a visibly smooth
 * bar without needing float anywhere in this file. */
constexpr int32_t kBarMax = 1000;

int32_t to_bar_value(kf_pet_millipercent mp) {
    return static_cast<int32_t>(mp / 100u);
}

/* One row: a name label above a bar, with a live percentage to its right.
 * `top_y` is this row's own top edge; everything else is placed relative
 * to it so the three calls below are just "the next row down". */
Row make_row(lv_obj_t *screen, const char *name, int16_t top_y) {
    Row row;

    lv_obj_t *name_label = lv_label_create(screen);
    lv_label_set_text(name_label, name);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 12, top_y);

    row.bar = lv_bar_create(screen);
    lv_obj_set_size(row.bar, 150, 12);
    lv_bar_set_range(row.bar, 0, kBarMax);
    lv_obj_align(row.bar, LV_ALIGN_TOP_LEFT, 12, top_y + 18);

    row.value_label = lv_label_create(screen);
    lv_obj_align(row.value_label, LV_ALIGN_TOP_LEFT, 168, top_y + 17);

    return row;
}

void update_row(const Row &row, kf_pet_millipercent mp) {
    lv_bar_set_value(row.bar, to_bar_value(mp), LV_ANIM_OFF);
    /* e.g. "92.3%" -- one decimal digit, matching kf/app.cpp's own
     * append_fixed1() convention for the constraint HUD, just via LVGL's
     * built-in printf-style label setter instead of hand-rolled digits:
     * this file is simulator-side presentation code, not Core, so
     * kf/poison.h's ban on libc string formatting does not apply here. */
    lv_label_set_text_fmt(row.value_label, "%u.%u%%",
                           static_cast<unsigned>(mp / 1000u),
                           static_cast<unsigned>((mp / 100u) % 10u));
}

void on_feed(lv_event_t *) { kf_pet_session_feed(); }
void on_play(lv_event_t *) { kf_pet_session_play(); }
void on_rest(lv_event_t *) { kf_pet_session_rest(); }

lv_obj_t *make_button(lv_obj_t *screen, const char *text, int16_t x,
                       lv_event_cb_t cb) {
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 70, 28);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, x, -8);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_style(button, &g_focus_style, LV_STATE_FOCUSED);
    return button;
}

} // namespace

void kf_pet_screen_init(void) {
    init_focus_style();

    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Pet");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    g.hunger = make_row(screen, "Hunger", 32);
    g.happiness = make_row(screen, "Happy", 84);
    g.energy = make_row(screen, "Energy", 136);

    make_button(screen, "Feed", 8, on_feed);
    make_button(screen, "Play", 85, on_play);
    make_button(screen, "Rest", 162, on_rest);

    /* No explicit lv_group_add_obj() here, deliberately -- an earlier
     * version of this file called it for all three buttons, matching the
     * old proof screen's pattern, and it was wrong. lv_button's own
     * lv_obj_class sets group_def = LV_OBJ_CLASS_GROUP_DEF_TRUE
     * (lv_button.c), which lv_obj_class_init() (lv_obj_class.c) reads to
     * auto-add every new button to whatever group is CURRENTLY DEFAULT the
     * moment it is created -- already true here, since kf_lvgl_input_init()
     * calls lv_group_set_default() before this function ever runs. Calling
     * lv_group_add_obj() again on an object already in that group does not
     * no-op: it internally calls lv_group_remove_obj() first (see
     * lv_group.c's own "be sure the object is removed from its current
     * group" comment), and removing the CURRENTLY FOCUSED object forces an
     * immediate refocus onto some other member before the object is
     * re-inserted at the list's tail. Three redundant add calls in a row
     * each independently perturbed focus and list order, and which button
     * ended up focused after all three was not "Feed" (the first one
     * created, the naive expectation) but whichever member the group's
     * internal remove/refocus/reinsert churn happened to land on --
     * confirmed directly via lv_obj_get_state() while diagnosing, not
     * guessed at. The buttons are already reachable the moment they're
     * created; nothing else needs to run. */
    if (lv_group_get_default() == nullptr) {
        KF_LOGW(TAG, "no default LVGL group -- the pet screen's buttons "
                     "will not be reachable with the keypad");
    }

    g.ready = true;
    kf_pet_screen_update();
}

void kf_pet_screen_update(void) {
    KF_ASSERT(g.ready,
              "kf_pet_screen_update called before kf_pet_screen_init");
    const kf_pet_state *state = kf_pet_session_state();
    update_row(g.hunger, state->hunger_mp);
    update_row(g.happiness, state->happiness_mp);
    update_row(g.energy, state->energy_mp);
}
