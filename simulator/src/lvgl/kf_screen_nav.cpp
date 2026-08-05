/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_screen_nav.h"

#include "kf_pet_info_screen.h"
#include "kf_pet_screen.h"

#include "kf/app.h"
#include "kf/hal/log.h"

#include <lvgl.h>

namespace {

constexpr const char *TAG = "screen-nav";

/* Fixed, ordered list of screens this build knows about. `root` is what
 * gets passed to lv_screen_load(); `update` is called only while that
 * screen is the active one. Home is index 0 on purpose -- B always jumps
 * there, and kf_screen_nav_init() loads it first -- matching kf_pet_
 * screen.h's existing "the screen a running build shows by default" role,
 * unchanged from every prior slice. */
struct ScreenEntry {
    lv_obj_t *root;
    void (*update)();
};

constexpr size_t kScreenCount = 2;
ScreenEntry g_screens[kScreenCount];
size_t g_active = 0;

void load(size_t index) {
    g_active = index;
    lv_screen_load(g_screens[g_active].root);
}

void advance() {
    load((g_active + 1u) % kScreenCount);
}

void go_home() {
    if (g_active != 0u) {
        load(0u);
    }
}

} // namespace

void kf_screen_nav_init(void) {
    /* Home's widgets go on lv_screen_active() -- the screen LVGL created
     * automatically at lv_init() time, still active right now since
     * nothing has called lv_screen_load() yet. Capture it before Info's
     * init runs, the same "read it once, right after it's known to be
     * true" approach kf_pet_screen.cpp's own blob positioning comment
     * already uses for a different value. */
    kf_pet_screen_init();
    g_screens[0] = {lv_screen_active(), kf_pet_screen_update};

    kf_pet_info_screen_init();
    g_screens[1] = {kf_pet_info_screen_root(), kf_pet_info_screen_update};

    g_active = 0;
    KF_LOGI(TAG, "%zu screens ready, starting on Home", kScreenCount);
}

void kf_screen_nav_frame(void) {
    const uint32_t pressed = kf_app_buttons_pressed();
    if (pressed & KF_BTN_MENU) {
        advance();
    } else if (pressed & KF_BTN_B) {
        go_home();
    }
    g_screens[g_active].update();
}

void kf_screen_nav_debug_advance(void) { advance(); }

void kf_screen_nav_debug_home(void) { go_home(); }

int kf_screen_nav_debug_index(void) { return static_cast<int>(g_active); }
