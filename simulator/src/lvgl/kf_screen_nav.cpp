/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_screen_nav.h"

#include "kf_pet_info_screen.h"

#include "../pet/kf_creature_screen.h"

#include "kf/app.h"
#include "kf/hal/log.h"

#include <lvgl.h>

namespace {

constexpr const char *TAG = "screen-nav";

/* Fixed, ordered list of screens this build knows about. `root` is what
 * gets passed to lv_screen_load() -- nullptr for a screen that draws
 * straight into the framebuffer itself instead of through an LVGL widget
 * tree (Home, the creature screen, since Task 4); see kf_screen_nav_
 * wants_lvgl() and load() below for the two places that null checks
 * matters. `update` is called only while that screen is the active one,
 * every frame, and now takes dt_ms -- Home's kf_creature_screen_frame()
 * needs a real elapsed-time value and nothing else supplies one; Info's
 * kf_pet_info_screen_update() ignores it via the update_info() wrapper
 * below. Home is index 0 on purpose -- B always jumps there, and
 * kf_screen_nav_init() loads it first -- matching kf_pet_screen.h's
 * original "the screen a running build shows by default" role, unchanged
 * from every prior slice even though a different screen now fills it. */
struct ScreenEntry {
    lv_obj_t *root;
    void (*update)(uint32_t dt_ms);
};

constexpr size_t kScreenCount = 2;
ScreenEntry g_screens[kScreenCount];
size_t g_active = 0;

/* kf_pet_info_screen_update() takes no arguments (nothing about Info is
 * time-based); this just drops dt_ms on the floor so it fits ScreenEntry's
 * one shared update() shape. */
void update_info_screen(uint32_t /*dt_ms*/) { kf_pet_info_screen_update(); }

void load(size_t index) {
    g_active = index;
    if (g_screens[index].root == nullptr) {
        /* Switching TO a screen that owns the framebuffer directly (Home):
         * it must repaint the whole panel in full right now, or it
         * inherits whatever LVGL last left in those pixels and then only
         * ever erases its own small moving rectangle out of that -- the
         * black-trail bug (docs/architecture/adr-0017-pet-screen.md:143-
         * 188) in a new shape. kf_creature_screen_enter() is exactly that
         * repaint; see its own header comment. */
        kf_creature_screen_enter();
        return;
    }
    /* Switching TO an LVGL screen (Info, today): LVGL's own dirty
     * tracking still believes this screen's pixels are exactly what it
     * last flushed, even if the creature screen has spent however long
     * drawing over them since -- LVGL has no way to know that happened,
     * since it never went through LVGL's draw path at all. Left alone,
     * the coming kf_lvgl_port_pump() would only redraw whatever LVGL's
     * OWN widget tree thinks changed, which can easily be nothing, and
     * leave creature debris on screen permanently.
     *
     * Both calls below are load-bearing, for two DIFFERENT reasons, not
     * one -- this is not belt-and-suspenders:
     *
     * - lv_obj_invalidate() here is a no-op the very FIRST time this runs:
     *   it returns early for an object that is not the active screen
     *   (lv_obj_pos.c), and Info is not active yet at this point in the
     *   call. That first full repaint instead comes from
     *   lv_screen_load() -> scr_load_internal()'s OWN trailing
     *   lv_obj_invalidate() call (lv_display.c), made once Info actually
     *   is the active screen.
     * - On every LATER switch back to Info, lv_screen_load() early-returns
     *   immediately when asked to load the screen that is already active
     *   (lv_display.c) -- scr_load_internal() never runs again, so its
     *   trailing invalidate never fires either. The explicit call here is
     *   what forces the repaint on THAT path.
     *
     * So the explicit call earns its keep on the second and later visits to
     * a given screen; the first visit is carried by lv_screen_load()'s own
     * internals. Order between the two calls does not matter -- both just
     * flag the object dirty for the next kf_lvgl_port_pump(), which has not
     * run yet either way. */
    lv_obj_invalidate(g_screens[index].root);
    lv_screen_load(g_screens[index].root);
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
    /* Home is the creature screen (Task 4): it owns the framebuffer
     * directly, so it gets no lv_obj_t root -- see ScreenEntry's own
     * comment. kf_creature_screen_init() already paints the field's
     * background itself (via kf_creature_screen_enter(), called before
     * kf_creature_screen_init() returns -- not its LAST step; the last
     * statement just sets a ready flag, but the whole panel is painted
     * before any frame ever runs either way), the same "screen looks right
     * from its very first frame" convention
     * kf_pet_screen_init()/kf_pet_info_screen_init() already used, so
     * nothing extra is needed here the way lv_screen_active() capture used
     * to be for the old LVGL Home.
     *
     * kf_pet_screen.cpp (bars + care-action buttons on an LVGL screen) is
     * NOT initialised here any more and is therefore unreachable from a
     * running build -- deliberately, not an oversight: the stats band that
     * replaces it on the real screen is an explicit later plan (see
     * kf_screen_nav.h's own header comment). It still exists, still
     * compiles, and is still exercised directly by headless_main.cpp's
     * run_pet_screen_check(), unaffected by any of this. */
    kf_creature_screen_init();
    g_screens[0] = {nullptr, kf_creature_screen_frame};

    kf_pet_info_screen_init();
    g_screens[1] = {kf_pet_info_screen_root(), update_info_screen};

    g_active = 0;
    KF_LOGI(TAG, "%zu screens ready, starting on Home", kScreenCount);
}

void kf_screen_nav_frame(uint32_t dt_ms) {
    const uint32_t pressed = kf_app_buttons_pressed();
    if (pressed & KF_BTN_MENU) {
        advance();
    } else if (pressed & KF_BTN_B) {
        go_home();
    }
    g_screens[g_active].update(dt_ms);
}

bool kf_screen_nav_wants_lvgl(void) { return g_screens[g_active].root != nullptr; }

void kf_screen_nav_debug_advance(void) { advance(); }

void kf_screen_nav_debug_home(void) { go_home(); }

int kf_screen_nav_debug_index(void) { return static_cast<int>(g_active); }
