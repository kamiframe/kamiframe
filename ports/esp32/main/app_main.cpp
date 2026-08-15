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
 *  ESP_PARTITION_MMAP() HAS BEEN CONFIRMED ON REAL SILICON, AT THE SIZE
 *  THIS BUILD ACTUALLY SHIPS. ADR 0033 gave this port a real partition
 *  table and an asset pack (`.kfpack`) mounted by mapping the `assets`
 *  partition directly rather than copying it into PSRAM; the same hardware
 *  session read mapped bytes back correctly first against the 1,156-byte
 *  hello_sprite pack, then against the full 556,488-byte creature_demo
 *  pack this firmware actually embeds (ports/esp32/main/CMakeLists.txt) --
 *  see docs/hal.md for both. If a sprite comes back wrong, or the mapping
 *  call itself faults, on the next flash, this is no longer the first
 *  place to look; check what changed since that session instead.
 *
 *  THIS BUILD'S HOME SCREEN NEEDS LUA -- A LOAD FAILURE BLANKS IT, NOT
 *  DEGRADES IT. KF_HOME_SCREEN defaults to lua, and under that build
 *  creature.lua's kf.screen("home") group declares the entire Home scene;
 *  kf_lua_home_screen_frame() (simulator/src/pet/kf_lua_home_screen.cpp)
 *  only commits when kf_lua_scene_declared_anything() is true, so a script
 *  that fails to load leaves Home with nothing committed -- an empty
 *  screen, not a C++ fallback. If a first flash shows a blank Home, look
 *  at the Lua load path first, not last.
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

#include "kf_app_post_frame.h"
#include "kf_dbg_bridge.h"
#include "kf_frame_loop.h"
#include "kf_lua_demo_creature_script.h"
#include "kf_lua_port.h"
#ifdef KF_ENABLE_LVGL
#include "kf_lvgl_port.h"
#endif
#include "kf_pet_session.h"
#include "kf_screen_nav.h"

/* KF_DEMO3D_MODE: 0 = normal (the pet). 1 = the 2.5D configuration, a lit 3D
 * creature in a small window inside a scene painted once. 2 = full-screen 3D.
 *
 * A LOOK-AT-IT BUILD, not a feature. It replaces the pet entirely for the
 * session and exists to answer one question the desktop model cannot: what
 * does software 3D actually look like, and cost, on this panel. The
 * measurements in simulator/src/headless (--stress3d) are all host-modelled;
 * this is the same renderer on real silicon.
 *
 *     idf.py -DKF_DEMO3D=1 build flash monitor    # 2.5D pet window
 *     idf.py -DKF_DEMO3D=2 build flash monitor    # full-screen 3D
 *     idf.py build flash monitor                  # back to the pet
 *
 * The pet's save in NVS is never touched by this build -- it does not run
 * the pet session at all -- so reflashing without the flag brings the
 * creature back exactly where it was, aged by however long the demo ran. */
#ifndef KF_DEMO3D_MODE
#define KF_DEMO3D_MODE 0
#endif

#if KF_DEMO3D_MODE
#include "kf/framebuffer.h"
#include "kf_soft3d.h"
#include <cmath>
#endif

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdio.h>

namespace {

#if KF_DEMO3D_MODE
/* One frame of the 3D demo, in place of the shared pet frame loop.
 *
 * Spins so the lighting sweeps across the faces -- a still lit blob and a
 * flat silhouette look far too similar to judge from, which is the whole
 * reason this exists rather than a screenshot.
 *
 * Logs its real measured cost once a second. Those numbers are the point:
 * every 3D figure this project has so far is a desktop model, and this is
 * the first time the rasteriser runs on the hardware it is being judged
 * for. Reports the geometry and raster halves apart, because per-triangle
 * setup and per-pixel fill scale with completely different things. */
void run_soft3d_demo_frame() {
    static float yaw = 0.0f;
    static float pitch = 0.0f;
    static float light_t = 0.0f;
    static uint64_t next_log_us = 0;
    static uint32_t frames = 0;
    static uint64_t sum_us = 0;

    constexpr kf_color kSceneColor = KF_RGB(24, 26, 34);

#if KF_DEMO3D_MODE == 2
    /* Full screen: the worst case, and the contrast that makes the 2.5D
     * number meaningful. Nothing moves the window here -- it is already
     * everything. */
    const kf_rect viewport = {0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT};
    static bool scene_painted = false;
#else
    /* 2.5D: a pet-sized window that DRIFTS around the screen, bouncing off
     * the edges. Moving it is not decoration -- a stationary window cannot
     * show whether the dirty-rectangle path keeps up when the changed region
     * is somewhere new every frame, which is exactly what a wandering
     * creature does. */
    constexpr int16_t kWin = 120;
    static float px = 20.0f;
    static float py = 60.0f;
    static float vx = 0.9f;
    static float vy = 0.7f;
    static bool scene_painted = false;

    px += vx;
    py += vy;
    if (px < 0.0f) { px = 0.0f; vx = -vx; }
    if (py < 0.0f) { py = 0.0f; vy = -vy; }
    if (px > static_cast<float>(KF_DISPLAY_WIDTH - kWin)) {
        px = static_cast<float>(KF_DISPLAY_WIDTH - kWin);
        vx = -vx;
    }
    if (py > static_cast<float>(KF_DISPLAY_HEIGHT - kWin)) {
        py = static_cast<float>(KF_DISPLAY_HEIGHT - kWin);
        vy = -vy;
    }
    const int16_t x0 = static_cast<int16_t>(px);
    const int16_t y0 = static_cast<int16_t>(py);
    const kf_rect viewport = {x0, y0, static_cast<int16_t>(x0 + kWin),
                              static_cast<int16_t>(y0 + kWin)};
#endif

    if (!scene_painted) {
        kf_color *fb = kf_fb_pixels();
        for (int i = 0; i < KF_FRAMEBUFFER_PIXELS; ++i) {
            fb[i] = kSceneColor;
        }
        const kf_rect all = {0, 0, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT};
        kf_fb_mark_dirty(all);
        scene_painted = true;
    }

    /* CLEAR THE WINDOW BEFORE DRAWING INTO IT. Without this the object
     * leaves a ghost trail of itself as it turns, which is exactly what the
     * first run on hardware showed.
     *
     * It is not a bug in the rasteriser: kf_soft3d_rasterize() fills
     * triangles and nothing else, deliberately, because a caller compositing
     * a creature over a drawn scene must be able to do so without the
     * renderer erasing what is underneath. Clearing is the caller's call,
     * and a demo on a flat background is the case where the caller wants it.
     *
     * `last` is cleared as well as the current window: when the window
     * moves, the pixels it VACATED still hold the previous frame's object
     * and nothing else would ever repaint them. Both rectangles are marked
     * dirty, which is 2 of the 8 the frame allows. */
    static kf_rect last = {0, 0, 0, 0};
    static bool have_last = false;
    kf_color *fb = kf_fb_pixels();
    auto clear_rect = [&](const kf_rect &r) {
        for (int16_t y = r.y0; y < r.y1; ++y) {
            kf_color *row = fb + static_cast<size_t>(y) * KF_DISPLAY_WIDTH;
            for (int16_t x = r.x0; x < r.x1; ++x) {
                row[x] = kSceneColor;
            }
        }
        kf_fb_mark_dirty(r);
    };
    if (have_last) {
        clear_rect(last);
    }
    clear_rect(viewport);
    last = viewport;
    have_last = true;

    /* The light travels while the object tumbles on two axes. A single-axis
     * spin under a fixed light is genuinely ambiguous -- it can read as a
     * flat shape with cycling colours. A highlight sweeping ACROSS the
     * surface while the silhouette changes independently is something only a
     * solid can do, and settles the "does this look 3D" question that no
     * timing number answers. */
    const float lx = -0.60f * std::cos(light_t);
    const float ly = 0.55f;
    const float lz = -0.60f - 0.35f * std::sin(light_t);
    kf_soft3d_set_light(lx, ly, lz);

    const uint64_t t0 = kf_time_mono_us();
    kf_soft3d_stats stats{};
    kf_soft3d_render(viewport, yaw, pitch, &stats);
    const uint64_t t1 = kf_time_mono_us();

    yaw += 2.0f;
    if (yaw >= 360.0f) {
        yaw -= 360.0f;
    }
    /* Deliberately not a multiple of the yaw step, so the pair never settles
     * into a short repeating cycle that looks like a canned animation. */
    pitch += 0.7f;
    if (pitch >= 360.0f) {
        pitch -= 360.0f;
    }
    light_t += 0.05f;
    if (light_t >= 6.2832f) {
        light_t -= 6.2832f;
    }

    sum_us += (t1 - t0);
    ++frames;
    if (t1 >= next_log_us) {
        /* Every FIVE seconds, not every one. Serial logging is the reason
         * the first run hitched: this line plus Core's own per-second budget
         * report is close to a kilobyte down a 115200 baud link, and that is
         * roughly 80ms of blocking UART -- far more than the 1ms of actual
         * 3D work it is reporting on. The measurement was disturbing the
         * thing it measured. */
        KF_LOGI("soft3d",
                "mode %d: %lu us/frame avg over %lu frames, %lu of %lu tris, "
                "%lu px, %lu spans",
                KF_DEMO3D_MODE,
                static_cast<unsigned long>(frames ? sum_us / frames : 0),
                static_cast<unsigned long>(frames),
                static_cast<unsigned long>(stats.tris_drawn),
                static_cast<unsigned long>(stats.tris_submitted),
                static_cast<unsigned long>(stats.pixels_written),
                static_cast<unsigned long>(stats.spans));
        next_log_us = t1 + 5000000ull;
        frames = 0;
        sum_us = 0;
    }
}
#endif /* KF_DEMO3D_MODE */
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

#if KF_DEMO3D_MODE
    KF_LOGW(TAG, "KF_DEMO3D_MODE=%d -- this build shows the software 3D "
                 "demo INSTEAD of the pet. The pet's save in NVS is not "
                 "touched; reflash without -DKF_DEMO3D to get it back.",
            KF_DEMO3D_MODE);
#endif

    uint64_t next_pet_log_us = kf_time_mono_us() + kPetLogIntervalUs;

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
         * in the same queue is handled here too, so the multiplier
         * kf_frame_loop_run() reads below already reflects it this same
         * iteration. This call has to stay outside kf_frame_loop_run(): it
         * needs to run before kf_app_frame(), which the shared function
         * does not call at all -- see that function's own header comment. */
        kf_dbg_bridge_frame();

        if (!kf_app_frame()) {
            break;
        }

        /* kf_app_frame() paces itself against KF_FRAME_BUDGET_US via
         * kf_time_delay_us() (see hakoniwaos/src/app.cpp) -- no extra
         * vTaskDelay needed here, matching sdl_main.cpp's own bare
         * while-loop shape on desktop.
         *
         * The shared sequence -- time handling, the pet session tick,
         * screen nav, LVGL's pump, Lua's frame and the scene commit -- lives
         * in kf_frame_loop_run() now (ADR 0058), not here, so this file and
         * sdl_main.cpp cannot independently drift the way they did before:
         * see that function's own header comment for the incident this
         * fixed, and this file's header comment above for the device half
         * of it. No hooks here -- the device has nothing to plug into the
         * one slot the shared function exposes (sdl_main.cpp's debug window
         * is desktop-only).
         *
         * Timed from OUTSIDE, across the whole call, for
         * kf_app_post_frame_us(): this brackets exactly the segment
         * kf_app_post_frame.h describes -- the work a PORT does after
         * kf_app_frame() returned above (pet session, screen nav, LVGL's
         * pump, Lua's frame). Task 7 reads this alongside cpu_us
         * (kf_frame_stats, timed separately inside kf_app_frame() by Core)
         * to see the two segments apart rather than folded into one number
         * that would answer neither "is the render path over budget" nor
         * "is the rest of the loop over budget" precisely. */
        const uint64_t post_frame_start_us = kf_time_mono_us();
#if KF_DEMO3D_MODE
        run_soft3d_demo_frame();
#else
        kf_frame_loop_run(kf_dbg_time_multiplier(), nullptr);
#endif
        const uint64_t now_us = kf_time_mono_us();
        g_post_frame_us = static_cast<uint32_t>(now_us - post_frame_start_us);

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
