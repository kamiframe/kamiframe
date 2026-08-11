/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point, and the first version of this file that is a whole
 * device rather than a proof of one.
 *
 * The path here: ADR 0020 got the HAL backends running for real, ADR 0025
 * got a live kf_pet_state ticking, ADR 0026 put a real DS3231 behind the
 * wall clock, and three slices landed together -- ADR 0027 (LVGL, so the pet
 * has a SCREEN), ADR 0028 (Lua, so the demo creature SCRIPT drives it), and
 * ADR 0029 (panel profiles, so the driver underneath is not welded to one
 * display module).
 *
 * This file calls the real thing, and nothing here is a stub:
 * kf_app_init()/kf_app_frame() (kf/app.h), kf_pet_session_init()/_frame()
 * (simulator/src/pet/), kf_lvgl_port_init()/_pump() and kf_screen_nav_init()/
 * _frame() (simulator/src/lvgl/), and kf_lua_port_init()/_frame()
 * (simulator/src/lua/). Those are the same entry points sdl_main.cpp drives
 * on desktop, in the same order, for the reasons that file's own comments
 * give: the pet session needs the storage/power/time HAL up first, LVGL's
 * memory pool needs kf_app_init()'s arenas up first, and the per-frame order
 * (pet session, then screen nav, then LVGL's pump) is what stops a screen
 * showing last frame's numbers.
 *
 * KF_DEMO_NONE, not KF_DEMO_SPRITE: now that LVGL owns the framebuffer, the
 * demo sprite would fight it for the same pixels with no coordination
 * between them -- see kf/demo.h's comment on KF_DEMO_NONE, and ADR 0013's
 * "Found after delivery" section, which diagnosed exactly this on desktop
 * before it could happen here. sdl_main.cpp made the identical switch for
 * the identical reason.
 *
 * ============================================================================
 *  WHAT HAS RUN ON HARDWARE, AND WHAT HAS NOT -- read before assuming more,
 *  or less, than this slice claims. This section used to say none of it had
 *  been flashed; a real hardware session on 2026-08-08 made that stale, so
 *  read what follows precisely -- some of this is now confirmed, some of it
 *  is still a clean cross-compile, and the two should not be blurred.
 *
 *  THE PET SCREEN HAS BEEN SEEN ON GLASS. That same session flashed this
 *  LVGL+pet-screen build to a real ESP32-S3 with the ILI9341 bring-up panel
 *  and drove it. It did not work first try, and every one of the following
 *  was a real bug found only by watching the real board, invisible on
 *  desktop: a DMA race in the display driver's byte-swap path duplicated
 *  bands and dropped the top of the frame; unchanged frames were being
 *  re-sent every tick until dirty rectangles were honoured; and the D-pad
 *  could not move focus off Feed, because a comment's claim about LVGL's
 *  default key handling was wrong and nobody had checked it against LVGL's
 *  own source until then. What remains is tearing on content that genuinely
 *  changes -- the ILI9341 module on hand exposes no TE line, a
 *  scanline-polling workaround was built and measured on real hardware, and
 *  it did not help (ADR 0032). Tearing is accepted, not fixed; choosing a
 *  panel that can synchronise with the host is now a stated criterion for
 *  the real board, not an assumption.
 *
 *  LUA IS PRESENT AND NOT CRASHING ON DEVICE. The same firmware, Lua linked
 *  in and the demo creature script running its own frame calls, stayed up
 *  through the whole session above -- screenshots pulled over KFDBG, buttons
 *  pressed, state queried, the display driver fixed and reflashed more than
 *  once. None of that would have kept working through a Lua init crash.
 *  What that does NOT confirm: nobody has independently watched the demo
 *  creature's own choices -- pet.feed()/play()/rest() called FROM Lua --
 *  reach the same live kf_pet_state the pet screen reads. Presence and
 *  non-crashing is verified; the creature actually driving observable
 *  behaviour on-device is not. Treat that link as open until someone
 *  watches it happen.
 *
 *  THE PANEL PROFILE THIS BUILD DEFAULTS TO (ILI9341) IS THE ONE THE SESSION
 *  ABOVE ACTUALLY DROVE -- "correct per the bring-up diagnostic" and
 *  "correct per this firmware" used to be two different claims; they are
 *  the same claim now.
 *
 *  ESP_PARTITION_MMAP() HAS NEVER BEEN CONFIRMED ON REAL FLASH, AND THIS IS
 *  THE BIGGEST OPEN QUESTION ON THIS LIST. ADR 0033 gave this port a real
 *  partition table and an asset pack (`.kfpack`) mounted by mapping the
 *  `assets` partition directly rather than copying it into PSRAM, but that
 *  work is build-verified only -- nobody has read a mapped sprite byte off
 *  a real chip yet. If a sprite comes back wrong, or the mapping call itself
 *  faults, on the next flash, look here first.
 *
 *  The pet screen needs no Lua: kf_pet_screen.cpp only reads
 *  kf_pet_session_state() and calls kf_pet_session_feed()/play()/rest(),
 *  the same C++ API this file already drove before either slice. So a Lua
 *  fault should degrade the creature's behaviour, not blank the screen --
 *  which is worth knowing when reading a first-flash failure.
 *
 *  kf_time_wall() IS backed by a real DS3231 (ADR 0026), hardware-verified
 *  since before the session above: the bring-up diagnostic confirmed the
 *  clock advancing across a genuine power cut on coin-cell power. So
 *  kf_pet_session_init()'s offline fast-forward has something real to
 *  fast-forward across, on a board with the cell fitted.
 * ============================================================================
 */

#include "kf/app.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "kf/scene.h"

#include "kf_app_post_frame.h"
#include "kf_dbg_bridge.h"
#include "kf_lua_demo_creature_script.h"
#include "kf_lua_port.h"
#include "kf_lua_scene.h"
#ifdef KF_ENABLE_LVGL
#include "kf_lvgl_port.h"
#endif
#include "kf_pet_session.h"
#include "kf_screen_nav.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdio.h>

namespace {
constexpr const char *TAG = "app_main";

/* How often to print the pet's state to the serial log, on top of the
 * screen this slice adds. Kept, not replaced: a screen needs a working
 * device in hand and good lighting, and the log has been the fastest way
 * to confirm the pet is actually ticking since ADR 0025, still true now
 * that there is somewhere else to look too. 10s is frequent enough to
 * watch hunger/happiness/energy move against the default decay rates (the
 * fastest, hunger, is 1042 mp/hour -- about 3 millipercent every 10s,
 * visible over a few minutes) without flooding the monitor. */
constexpr uint64_t kPetLogIntervalUs = 10ull * 1000ull * 1000ull;

/* Set once per loop iteration by app_main(), bracketing exactly the segment
 * kf_app_post_frame.h's own comment describes -- the work a PORT does after
 * kf_app_frame() returns. 0 before the first iteration completes. No lock:
 * written and read on the same thread (the main frame-loop thread), always
 * written before KFDBG STATE could possibly read it within one iteration --
 * same single-thread reasoning kf_dbg_bridge.h already gives for
 * kf_dbg_input_mask()/kf_dbg_time_multiplier(), just running the other
 * direction (a port exposing state to the bridge, not the reverse). */
uint32_t g_post_frame_us = 0;

void log_pet_state() {
    const kf_pet_state *pet = kf_pet_session_state();
    KF_LOGI(TAG,
            "pet: stage=%d hunger=%lu.%02lu%% happy=%lu.%02lu%% "
            "energy=%lu.%02lu%% base_trait=%u",
            static_cast<int>(pet->stage),
            static_cast<unsigned long>(pet->hunger_mp / 1000u),
            static_cast<unsigned long>((pet->hunger_mp / 10u) % 100u),
            static_cast<unsigned long>(pet->happiness_mp / 1000u),
            static_cast<unsigned long>((pet->happiness_mp / 10u) % 100u),
            static_cast<unsigned long>(pet->energy_mp / 1000u),
            static_cast<unsigned long>((pet->energy_mp / 10u) % 100u),
            static_cast<unsigned>(pet->base_trait));
}

} // namespace

/* Definition for kf_app_post_frame.h's declaration -- linkage (extern "C")
 * comes from that header's own declaration, already seen by this point via
 * the #include above; nothing further needed here for it to match. */
uint32_t kf_app_post_frame_us(void) { return g_post_frame_us; }

extern "C" void app_main(void) {
    /* Kept from ADR 0019's hello-world verbatim: this banner is what Chris's
     * real Wokwi run already proved boots correctly, and there is no reason
     * to touch working, verified output while adding unrelated code below
     * it. */
    printf("kamiframe: hello from ESP-IDF\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("kamiframe: chip=%s cores=%d revision=%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("kamiframe: flash=%" PRIu32 "MB\n", flash_size / (1024u * 1024u));
    }

    printf("kamiframe: free heap=%" PRIu32 " bytes\n", esp_get_free_heap_size());

    /* Real HAL backends from here on -- see this file's header comment for
     * exactly what "real" does and does not mean yet. KF_DEMO_NONE, not
     * KF_DEMO_SPRITE: LVGL owns the framebuffer now -- see this file's
     * header comment. */
    KF_LOGI(TAG, "starting kf_app_init (real ESP32 HAL backends)");
    kf_app_init(KF_DEMO_NONE);

    /* Same ordering sdl_main.cpp uses, and the same reason: this only
     * needs the storage/power/time HAL, already up as of kf_app_init()
     * above. First real on-device call -- a fresh pet if NVS has no save
     * yet, or a loaded one fast-forwarded by however long kf_time_wall()
     * says has passed (see this file's header comment on ADR 0026). */
    KF_LOGI(TAG, "starting kf_pet_session_init (real NVS-backed pet)");
    kf_pet_session_init();
    log_pet_state();

    /* LVGL, only under -DKF_ENABLE_LVGL=ON (ADR 0045 -- Info moved to a
     * kf.screen() group over the retained scene, so a default build does
     * not link LVGL at all; the option keeps the code and its 256 KB
     * PSRAM arena available for the LVGL-vs-custom-engine evaluation
     * CLAUDE.md names as deliberately deferred). Then every screen this
     * build has -- Home's init function reads kf_pet_session_state() the
     * moment it runs, so kf_pet_session_init() above has to have already
     * run. */
#ifdef KF_ENABLE_LVGL
    KF_LOGI(TAG, "starting kf_lvgl_port_init (KF_ENABLE_LVGL=ON)");
    kf_lvgl_port_init();
#endif
    kf_screen_nav_init();

    /* Lua comes up last -- same ordering and same reason sdl_main.cpp uses
     * (see its own comment on this call): its allocator's one block comes
     * from KF_ARENA_LUA, already carved out above, and its pet.* binding
     * (ADR 0016) reads kf_pet_session_state(), which must already exist.
     *
     * Last on purpose rather than by accident: a script that fails to load
     * is not fatal to the rest of the firmware. It runs with no Lua this
     * session, logged loudly by kf_lua_port_init() itself, and everything
     * already initialised above -- including the screen -- keeps working.
     * That is the failure mode you want on a first flash. */
    KF_LOGI(TAG, "starting kf_lua_port_init (real demo creature script)");
    kf_lua_port_init(kKfLuaDemoCreatureScriptSource,
                      kKfLuaDemoCreatureScriptChunkName);

    /* Last of all the *_init() calls, and deliberately so: STATE's reply
     * reads kf_pet_session_state(), SHOT reads kf_fb_pixels() (up since
     * kf_app_init() above), and PING reads KF_PANEL_PROFILE -- every one of
     * those needs to already be real by the time a command could possibly
     * arrive, which is only ever inside the loop below. See ADR 0030 for
     * the full protocol and docs/architecture/adr-0030-serial-debug-bridge.md's
     * "Shipping a build with this off" section for the one-flag way to
     * remove this from a non-developer build. */
    KF_LOGI(TAG, "starting kf_dbg_bridge_init (serial debug bridge)");
    kf_dbg_bridge_init();

    uint64_t next_pet_log_us = kf_time_mono_us() + kPetLogIntervalUs;

    /* Tracked here, not left to kf_pet_session_frame()'s own internal
     * real-time tracking, because KFDBG MULT's time multiplier
     * (kf_dbg_time_multiplier()) has to scale ONLY the delta fed to the
     * pet session -- not LVGL's tick (kf_lvgl_port_pump()) or Lua's frame
     * delta (kf_lua_port_frame()), both of which stay real-time so
     * animation and script frame-rate semantics are unaffected. This
     * mirrors sdl_main.cpp's identical treatment of
     * kf_sdl_debug_window_time_multiplier() exactly -- see that file's own
     * comment on its last_frame_us/real_dt_ms/multiplier dance for the
     * full reasoning, including the correctness trap it avoids: passing a
     * non-zero synthetic value every frame (instead of 0 = "use your own
     * real-time tracking") sidesteps kf_pet_session_frame() only updating
     * its internal last-call timestamp on the `dt_ms == 0` path, which
     * would otherwise leave that timestamp stale across a multiplier
     * change and double-count the next 0-argument call. */
    uint64_t last_frame_us = 0;

    KF_LOGI(TAG, "running (kf_app_frame loop; no quit condition on device)");
    for (;;) {
        /* kf_dbg_bridge_frame() runs BEFORE kf_app_frame() specifically so
         * a KFDBG BTN/BTNHOLD command already sitting in the queue affects
         * THIS iteration's kf_input_poll() (called from inside
         * kf_app_frame(), first thing) -- "for the next frame," as the
         * protocol spec puts it, means the very next one, not the one
         * after. Non-blocking either way: see kf_dbg_bridge.h's own "WHY A
         * BACKGROUND TASK" comment for why this never stalls the loop
         * regardless of where it sits in it. A KFDBG MULT command sitting
         * in the same queue is handled here too, so the multiplier read
         * below already reflects it this same iteration. */
        kf_dbg_bridge_frame();

        if (!kf_app_frame()) {
            break;
        }

        /* kf_app_frame() paces itself against KF_FRAME_BUDGET_US via
         * kf_time_delay_us() (see hakoniwaos/src/app.cpp) -- no extra
         * vTaskDelay needed here, matching sdl_main.cpp's own bare
         * while-loop shape on desktop.
         *
         * real_dt_ms * multiplier feeds ONLY kf_pet_session_frame() --
         * see this loop's header comment above. Every 0 argument below
         * still means "use your own real-elapsed-time tracking" (see
         * kf_lvgl_port.h, kf_lua_port.h). kf_screen_nav_frame() is the one
         * exception: it has no internal real-time tracker of its own (the
         * creature screen's wander just advances by whatever dt_ms it is
         * handed), so it gets real_dt_ms directly, UN-multiplied -- the
         * creature's walk is presentation, not pet decay, and stays
         * real-time the same way LVGL's tick and Lua's frame delta do.
         *
         * Ordering matches sdl_main.cpp's frame-ordering comment exactly,
         * for the same two reasons. kf_pet_session_frame() and
         * kf_screen_nav_frame() both run before kf_lvgl_port_pump() (under
         * -DKF_ENABLE_LVGL=ON): the active screen has this frame's numbers
         * pushed into it before pump's lv_timer_handler() would redraw and
         * flush. ADR 0045 removed the kf_screen_nav_wants_lvgl() guard that
         * call used to need: every screen this build can show (Home, Info)
         * is a kf.screen() group over the retained scene now, so LVGL, when
         * built in at all, has nothing left to pump for -- the call below is
         * unconditional on which screen is active, only on KF_ENABLE_LVGL
         * itself. And the pet session runs before kf_lua_port_frame(), so
         * the script's pet.hunger() and friends see this frame's elapsed
         * time already applied. */
        const uint64_t now_us = kf_time_mono_us();
        const uint32_t real_dt_ms =
            last_frame_us == 0u
                ? 0u
                : static_cast<uint32_t>((now_us - last_frame_us) / 1000u);
        last_frame_us = now_us;
        const uint32_t multiplier = kf_dbg_time_multiplier();

        /* Brackets exactly the segment kf_app_post_frame.h describes: the
         * work a PORT does after kf_app_frame() returned above -- pet
         * session, screen nav (which is what actually draws the creature;
         * see hakoniwaos/src/app.cpp's kf_draw_counters_reset() comment,
         * ADR 0036, for why Core cannot see or time this itself), LVGL's
         * pump when it runs, and Lua's own frame. Task 7 reads this
         * alongside cpu_us (kf_frame_stats, timed separately inside
         * kf_app_frame() by Core) to see the two segments apart rather than
         * folded into one number that would answer neither "is the render
         * path over budget" nor "is the rest of the loop over budget"
         * precisely. */
        const uint64_t post_frame_start_us = kf_time_mono_us();

        kf_pet_session_frame(real_dt_ms * multiplier);
        kf_screen_nav_frame(real_dt_ms);
#ifdef KF_ENABLE_LVGL
        kf_lvgl_port_pump(0);
#endif

        /* kf_lua_port_frame(0): same "0 means real elapsed time" convention
         * as kf_pet_session_frame() would use without a multiplier, tracked
         * internally the same way kf_lvgl_port_pump()'s is -- Lua's frame
         * delta deliberately does not get the multiplier folded in, per
         * this loop's header comment. */
        kf_lua_port_frame(0);

        /* kf_scene_commit() belongs to the frame loop, not the Lua binding
         * (Task 3 of docs/superpowers/plans/2026-08-12-lua-game-layer.md)
         * -- same ordering and the same reasoning as sdl_main.cpp's
         * identical call, including the kf_lua_scene_declared_anything()
         * guard: without it, the very first frame after boot would paint
         * one solid KF_BLACK frame over whatever the creature screen or
         * LVGL just drew, because hakoniwaos/src/scene.cpp's own
         * g_force_full_redraw starts true and nothing here ever calls
         * kf_scene_reset() (that is Task 4's job). See kf_lua_scene.h's
         * own comment on this predicate for the full reasoning; the demo
         * creature script still only logs, so this stays a no-op device-
         * side for the whole of this task. */
        if (kf_lua_scene_declared_anything()) {
            kf_scene_commit();
        }

        g_post_frame_us = static_cast<uint32_t>(kf_time_mono_us() -
                                                  post_frame_start_us);

        if (now_us >= next_pet_log_us) {
            log_pet_state();
            next_pet_log_us = now_us + kPetLogIntervalUs;
        }
    }

    /* Unreachable in practice: esp_input.cpp's kf_input_poll() hardcodes
     * quit_requested = false (there is no hardware "quit" concept), so
     * g.running in app.cpp never flips false on this backend -- `for (;;)`
     * with an explicit `if (!kf_app_frame()) break;` above still only ever
     * exits through that same, never-taken path. Kept anyway, the same way
     * sdl_main.cpp keeps its own shutdown call after a loop that can exit --
     * correct shape if that ever changes, dead code if it doesn't, and
     * either way cheaper than leaving it out and being wrong later.
     * Shutdown is the exact reverse of the init order above -- the debug
     * bridge, then Lua, then LVGL, then the pet session, then the app --
     * which for everything below kf_dbg_bridge_shutdown() is also the order
     * sdl_main.cpp uses. Lua and LVGL only touch their own arena blocks, but
     * kf_pet_session_shutdown() still needs the storage HAL that
     * kf_app_shutdown() is about to tear down. */
    kf_dbg_bridge_shutdown();
    kf_lua_port_shutdown();
#ifdef KF_ENABLE_LVGL
    kf_lvgl_port_shutdown();
#endif
    kf_pet_session_shutdown();
    kf_app_shutdown();
}
