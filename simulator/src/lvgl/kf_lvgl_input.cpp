/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_lvgl_input.h"

#include "kf/app.h"
#include "kf/types.h"

namespace {

lv_group_t *g_group = nullptr;
uint32_t g_last_key = 0;

/* One button, one key, checked in a fixed priority order. A physical keypad
 * only ever has one thing "focused" at a time, and the buttons this project
 * has (kf/types.h's kf_button) map onto LVGL's keypad keys directly enough
 * that nothing fancier -- chords, multi-key -- is worth building for a
 * five-widget menu layer. */
uint32_t button_to_key(uint32_t buttons) {
    /* PREV/NEXT, not UP/DOWN/LEFT/RIGHT, and this was a real bug found on
     * hardware: with the arrow keys, focus could never move at all.
     *
     * LVGL's keypad handling only calls lv_group_focus_next()/_prev() for
     * LV_KEY_NEXT and LV_KEY_PREV (indev/lv_indev.c, indev_keypad_proc()).
     * Every other key -- including all four arrows -- is delivered to the
     * FOCUSED WIDGET instead, and lv_button ignores them. So mapping the
     * D-pad to arrows meant every press was correctly read, correctly
     * debounced, correctly handed to LVGL, and then silently dropped by the
     * focused button. The symptom was a pet screen stuck on Feed forever,
     * with Play and Rest unreachable.
     *
     * A previous comment here asserted the opposite -- that arrows move
     * focus "by LVGL's own default group behaviour". That was wrong, and
     * wrong in the confident direction, which is why nobody re-checked it
     * until a real device made it visible. It is checked now: see the
     * function reference above. */
    if (buttons & (KF_BTN_UP | KF_BTN_LEFT)) {
        return LV_KEY_PREV;
    }
    if (buttons & (KF_BTN_DOWN | KF_BTN_RIGHT)) {
        return LV_KEY_NEXT;
    }
    if (buttons & KF_BTN_A) {
        return LV_KEY_ENTER;
    }
    if (buttons & KF_BTN_B) {
        return LV_KEY_ESC;
    }
    /* KF_BTN_MENU deliberately maps to nothing here as of ADR 0022:
     * kf_screen_nav.cpp now owns MENU exclusively, for switching which
     * LVGL screen is loaded, reading kf_app_buttons_pressed() directly
     * rather than going through this indev's keypad protocol. Before that,
     * MENU doubled as LV_KEY_NEXT (cycle focus) here -- redundant even
     * then, since LV_KEY_UP/DOWN/LEFT/RIGHT above already move focus to
     * the previous/next group member by LVGL's own default group
     * behaviour (see lv_group_focus_next()/lv_group_focus_prev()), so the
     * D-pad alone already covers "move to the next widget" within
     * whichever screen is active. Removing MENU's redundant mapping here
     * avoids it doing two different navigational things at once (cycle
     * focus AND, now, switch screens) depending on what LVGL's group
     * state happened to be. Core's own HUD toggle on KF_BTN_MENU
     * (kf/app.h) is unrelated and unaffected either way -- see ADR 0017's
     * "Found after delivery" section for why that sharing is accepted,
     * unchanged by this file. */
    return 0u;
}

void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    const uint32_t held = kf_app_buttons_held();
    const uint32_t key = button_to_key(held);

    if (key != 0u) {
        g_last_key = key;
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->key = g_last_key;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

} // namespace

lv_group_t *kf_lvgl_input_init(void) {
    g_group = lv_group_create();
    lv_group_set_default(g_group);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_group(indev, g_group);

    return g_group;
}
