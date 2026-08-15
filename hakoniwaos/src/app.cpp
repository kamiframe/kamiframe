/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * One frame, and the accounting that goes with it.
 *
 * Note what is absent: there is no loop here. kf_app_frame() runs exactly one
 * frame and returns. Whichever backend is linked owns the loop. See kf/app.h.
 */

#include "kf/app.h"

#include "kf/arena.h"
#include "kf/assets.h"
#include "kf/blit.h"
#include "kf/budget.h"
#include "kf/font.h"
#include "kf/framebuffer.h"
#include "kf/hal/audio.h"
#include "kf/hal/display.h"
#include "kf/hal/entropy.h"
#include "kf/hal/input.h"
#include "kf/hal/log.h"
#include "kf/hal/power.h"
#include "kf/hal/storage.h"
#include "kf/hal/time.h"
#include "kf/demo.h"
#include "kf/settings.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "app";

/* Rolling window for the frame-time summary. 256 frames is a few seconds at
 * 30fps: long enough for p99 to mean something, short enough to react. */
constexpr int kWindow = 256;

struct AppState {
    bool initialised = false;
    bool running = false;

    uint64_t frame_index = 0;
    uint64_t frame_start_us = 0;

    kf_frame_stats last{};

    uint32_t window[kWindow] = {};
    int window_count = 0;
    int window_next = 0;

    uint64_t total_frames = 0;
    uint64_t over_budget_frames = 0;
    uint32_t worst_us = 0;

    uint64_t last_report_us = 0;

    /* Debounced button state, computed in core so the device and the
     * simulator feel the same. */
    uint32_t buttons_stable = 0;
    uint32_t buttons_candidate = 0;
    uint64_t candidate_since_us = 0;
    uint32_t buttons_pressed_edge = 0;

    /* The constraint HUD from ADR 0006's "Later" section. Off by default and
     * no longer bound to any button -- it is a development affordance, not
     * something the device shows a player, and it redraws every frame, which
     * is a visible ripple on a panel with no tearing-effect signal. A backend
     * that wants it drives it through kf_app_set_hud_visible(). */
    bool hud_visible = false;
};

AppState g;

/* Physical switches bounce for a few milliseconds when pressed. 8ms is the
 * usual safe figure. Doing this in core rather than per-backend means the
 * simulator has the same input latency as the hardware, which matters because
 * input latency is something you tune by feel. */
constexpr uint64_t kDebounceUs = 8000;

void debounce(const kf_input_raw &raw) {
    if (raw.buttons != g.buttons_candidate) {
        g.buttons_candidate = raw.buttons;
        g.candidate_since_us = raw.sampled_at_us;
        return;
    }
    if (g.buttons_candidate != g.buttons_stable &&
        raw.sampled_at_us - g.candidate_since_us >= kDebounceUs) {
        const uint32_t previous = g.buttons_stable;
        g.buttons_stable = g.buttons_candidate;
        g.buttons_pressed_edge = g.buttons_stable & ~previous;
    } else {
        g.buttons_pressed_edge = 0;
    }
}

/* Device draw time from the pixel count, not from the host clock. See the
 * KF_DRAW_* rates in budget.h and the comment above them. */
uint32_t estimate_draw_us(const kf_draw_counters &c) {
    const uint32_t opaque = c.opaque_pixels / KF_DRAW_OPAQUE_PX_PER_US;
    const uint32_t keyed = c.keyed_pixels / KF_DRAW_KEYED_PX_PER_US;
    return opaque + keyed;
}

/* What the panel link would cost for `bytes`, in microseconds.
 *
 * This is the number your desktop reports as zero and the device cannot
 * escape. See KF_DISPLAY_SPI_HZ in budget.h for the arithmetic. */
uint32_t estimate_transfer_us(size_t bytes, uint32_t link_bytes_per_second) {
    if (link_bytes_per_second == 0u || bytes == 0u) {
        return 0u;
    }
    const uint64_t us =
        (static_cast<uint64_t>(bytes) * 1000000ull) / link_bytes_per_second;
    return static_cast<uint32_t>(us);
}

void record(uint32_t total_us) {
    g.window[g.window_next] = total_us;
    g.window_next = (g.window_next + 1) % kWindow;
    if (g.window_count < kWindow) {
        g.window_count++;
    }
    g.total_frames++;
    if (total_us > g.worst_us) {
        g.worst_us = total_us;
    }
}

/* Insertion sort over at most 256 entries, into the per-frame scratch arena.
 * Called once a second for the report, never in the hot path. */
uint32_t percentile(int pct) {
    if (g.window_count == 0) {
        return 0u;
    }
    const int n = g.window_count;
    uint32_t *sorted = static_cast<uint32_t *>(
        kf_arena_alloc(KF_ARENA_SCRATCH, sizeof(uint32_t) * static_cast<size_t>(n),
                       alignof(uint32_t)));
    memcpy(sorted, g.window, sizeof(uint32_t) * static_cast<size_t>(n));

    for (int i = 1; i < n; ++i) {
        const uint32_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    int index = (n * pct) / 100;
    if (index >= n) {
        index = n - 1;
    }
    return sorted[index];
}

/* fps, in tenths, from a frame period in microseconds -- integer only.
 * hakoniwaos/ stays float-free (kf/budget.h's own reasoning: the FPU is
 * for Lua, Core stays exact and cheap); this used to be `1000000.0 /
 * period_us`, a genuine floating-point division that slipped into this
 * diagnostic log line, the one place in hakoniwaos/src/ that ever had
 * one. 10,000,000 / period_us is fps * 10 without ever leaving integers;
 * the caller splits it back into whole and tenths for "%2u.%1u". Fits
 * uint32_t comfortably for any period_us this file logs (sub-millisecond
 * periods would need ~4.3s to overflow it, far outside any real frame
 * time). Zero in, zero out -- same "no frame yet" convention the float
 * version's `? : 0.0` had. */
uint32_t fps_x10_from_us(uint32_t period_us) {
    return period_us ? (10000000u / period_us) : 0u;
}

/* --------------------------------------------------------------------------
 * Constraint HUD -- ADR 0006's "Later" section, now.
 *
 * Hand-rolled string building rather than snprintf: core has no heap, this
 * is a handful of digits, and a target that is not glibc is not somewhere
 * to trust a general formatter blindly. kf/poison.h would not even let this
 * file pull in <cstdio> for it.
 * -------------------------------------------------------------------------- */

char *append_str(char *dst, const char *end, const char *s) {
    while (*s != '\0' && dst < end) {
        *dst++ = *s++;
    }
    return dst;
}

char *append_uint(char *dst, const char *end, uint32_t v) {
    char digits[10];
    int n = 0;
    if (v == 0u) {
        digits[n++] = '0';
    }
    while (v > 0u && n < static_cast<int>(sizeof(digits))) {
        digits[n++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && dst < end) {
        *dst++ = digits[--n];
    }
    return dst;
}

/* `tenths` is already scaled by 10, e.g. pass fps*10 to print one decimal. */
char *append_fixed1(char *dst, const char *end, uint32_t tenths) {
    dst = append_uint(dst, end, tenths / 10u);
    if (dst < end) {
        *dst++ = '.';
    }
    if (dst < end) {
        *dst++ = static_cast<char>('0' + (tenths % 10u));
    }
    return dst;
}

/* Every HUD field (fps, us, percent, rect count, arena bytes) varies in digit
 * width frame to frame. kf_text_draw only paints cells for the characters it
 * is given -- that IS the "clear whatever was there" step (see its comment in
 * kf/font.cpp) -- so a line that got shorter than last frame leaves the old
 * tail's glyphs sitting in the framebuffer forever, uncleared, because
 * nothing ever draws over those cells again. Padding every line out to a
 * fixed width with spaces means this frame's draw always covers last frame's
 * worst case, at the cost of a few extra cheap background-only cells. */
constexpr int kHudLineWidth = 40;

char *pad_to(char *dst, const char *end, const char *line_start, int width) {
    while ((dst - line_start) < width && dst < end) {
        *dst++ = ' ';
    }
    return dst;
}

/* Drawn between the demo's draw call and this frame's own accounting, using
 * LAST frame's stats -- this frame cannot know its own total cost until
 * after it finishes, the same way a runner cannot time their own race
 * before crossing the line. One frame of lag on a HUD redrawn 30 times a
 * second is not something a human notices. */
void draw_hud(void) {
    constexpr kf_color kHudFg = KF_RGB(230, 235, 245);
    constexpr kf_color kHudBg = KF_RGB(20, 22, 32);

    /* Zero-initialised, not just declared: an uninitialised char[48] is fine
     * in practice (only the bytes up to the final NUL are ever read), but
     * GCC's -Wmaybe-uninitialized cannot see that from the pointer
     * arithmetic alone and flags `end` as possibly-uninitialized without
     * this. Cheap, and it settles the analysis instead of arguing with it. */
    char line[48] = {};
    const char *end = line + sizeof(line) - 1;
    char *w;

    w = line;
    w = append_str(w, end, "F");
    w = append_fixed1(w, end,
                       g.last.total_us > 0u
                           ? static_cast<uint32_t>(10000000ull / g.last.total_us)
                           : 0u);
    w = append_str(w, end, "FPS ");
    w = append_uint(w, end, g.last.total_us);
    w = append_str(w, end, "US D");
    w = append_uint(w, end, g.last.dirty_percent);
    w = append_str(w, end, "% R");
    w = append_uint(w, end, g.last.dirty_rect_count);
    if (g.last.over_budget) {
        w = append_str(w, end, " OVER");
    }
    w = pad_to(w, end, line, kHudLineWidth);
    *w = '\0';
    kf_text_draw(0, 0, line, kHudFg, kHudBg);

    /* One line per arena. Names are lowercase in kf/arena.cpp; the font is
     * uppercase-only (see kf/font.h), so upper-case them here rather than
     * widen the font for one call site. */
    constexpr kf_arena_id kArenas[] = {KF_ARENA_FRAMEBUFFER, KF_ARENA_SCRATCH,
                                       KF_ARENA_LUA, KF_ARENA_ASSETS};
    for (size_t i = 0; i < sizeof(kArenas) / sizeof(kArenas[0]); ++i) {
        const kf_arena_stats *s = kf_arena_get_stats(kArenas[i]);
        w = line;
        for (const char *n = s->name; *n != '\0' && w < end; ++n) {
            *w++ = static_cast<char>((*n >= 'a' && *n <= 'z')
                                          ? (*n - 'a' + 'A')
                                          : *n);
        }
        w = append_str(w, end, " ");
        w = append_uint(w, end,
                        static_cast<uint32_t>(s->high_water_bytes / 1024u));
        w = append_str(w, end, "/");
        w = append_uint(w, end,
                        static_cast<uint32_t>(s->capacity_bytes / 1024u));
        w = append_str(w, end, "K");
        w = pad_to(w, end, line, kHudLineWidth);
        *w = '\0';
        kf_text_draw(0,
                     static_cast<int16_t>(KF_FONT_CELL_H *
                                           (1 + static_cast<int>(i))),
                     line, kHudFg, kHudBg);
    }
}

} // namespace

void kf_app_init(kf_demo_mode mode) {
    KF_ASSERT(!g.initialised, "kf_app_init called twice");

    KF_LOGI(TAG, "HakoniwaOS starting");

    /* Arenas first: everything else allocates from them. */
    kf_arena_init_all();

    KF_ASSERT(kf_time_init() == KF_OK, "time HAL failed to start");
    KF_ASSERT(kf_display_init() == KF_OK, "display HAL failed to start");
    KF_ASSERT(kf_input_init() == KF_OK, "input HAL failed to start");
    KF_ASSERT(kf_store_init() == KF_OK, "storage HAL failed to start");
    KF_ASSERT(kf_power_init() == KF_OK, "power HAL failed to start");
    /* Always KF_OK, same as kf_time_init() with no RTC wired -- a backend
     * with no speaker, buzzer or amp still starts cleanly; it is
     * kf_audio_tone() that reports KF_ERR_UNAVAILABLE later, per call, not
     * this init. See kf/hal/audio.h's own header comment. */
    KF_ASSERT(kf_audio_init() == KF_OK, "audio HAL failed to start");
    /* The persisted, cross-pet volume setting (kf/settings.h) -- loaded
     * once, here, right after the audio HAL itself comes up (and after
     * storage, already initialised above) so every sound this process ever
     * plays, starting with the very first one, obeys whatever the owner
     * last saved on the Settings screen. kf_settings_load() always returns
     * KF_OK on a fresh device (falls back to KF_SETTINGS_DEFAULT_VOLUME,
     * see that function's own header comment), so there is nothing to
     * assert against here the way kf_audio_init() itself is asserted --
     * only kf_store_init() failing outright would matter, and that is
     * already asserted above. */
    {
        kf_settings settings = kf_settings_default();
        (void)kf_settings_load(&settings);
        kf_audio_set_volume(static_cast<kf_volume_level>(settings.volume));
        /* And the brightness, from the same load. Safe here because
         * kf_display_init() is the FIRST of these inits, several lines
         * above -- the panel and its PWM channel are already up, so this is
         * a real restore rather than a no-op that looks like one.
         *
         * Return deliberately ignored: KF_ERR_UNAVAILABLE is the correct,
         * expected answer on a panel whose backlight is not
         * software-controllable (the ILI9341's LED pin is soldered to 3V3),
         * and treating a device preference as unappliable there would be
         * wrong -- it is stored for whatever panel comes next. See
         * kf_lua_port_apply_brightness() for the same reasoning at the
         * write end. */
        (void)kf_display_set_backlight(
            kf_settings_brightness_duty(settings.brightness));
    }
    /* Before kf_demo_init() below: the demo looks up sprites by name via
     * kf_assets_get(), so the pack must already be mounted and parsed. */
    KF_ASSERT(kf_assets_init() == KF_OK, "assets HAL failed to start");

    const kf_display_caps *caps = kf_display_get_caps();
    KF_ASSERT(caps != nullptr, "display backend returned no capabilities");

    /* budget.h describes the panel core allocates for; caps describes the
     * panel that is actually attached. A future e-ink or mono variant makes
     * these diverge legitimately, and that is the day this check becomes a
     * real branch rather than an assert. */
    KF_ASSERT(caps->width == KF_DISPLAY_WIDTH &&
                  caps->height == KF_DISPLAY_HEIGHT,
              "Display backend reports %ux%u but kf/budget.h is built for "
              "%dx%d. A backend for a different panel needs core to handle "
              "capability-driven sizing, which it does not do yet.",
              caps->width, caps->height, KF_DISPLAY_WIDTH, KF_DISPLAY_HEIGHT);
    KF_ASSERT(caps->format == KF_PIXFMT_RGB565,
              "Display backend is not RGB565. Core only speaks RGB565 today.");

    kf_fb_init();

    uint32_t seed = 0;
    KF_ASSERT(kf_entropy(&seed, sizeof(seed)) == KF_OK,
              "entropy HAL failed to start");

    kf_demo_init(seed, mode);

    const kf_wall_time now = kf_time_wall();
    KF_LOGI(TAG, "wall clock %s (epoch %lld)",
            now.valid ? "valid" : "UNSET",
            static_cast<long long>(now.epoch_seconds));
    KF_LOGI(TAG, "budget: %d fps, %" PRIu32 " us per frame, link %" PRIu32 " bytes/s",
            KF_TARGET_FPS, static_cast<uint32_t>(KF_FRAME_BUDGET_US),
            caps->link_bytes_per_second);

    g.last_report_us = kf_time_mono_us();
    g.initialised = true;
    g.running = true;
}

bool kf_app_frame(void) {
    KF_ASSERT(g.initialised, "kf_app_frame before kf_app_init");

    g.frame_start_us = kf_time_mono_us();

    /* Everything allocated last frame is gone. Nothing may survive here. */
    kf_arena_reset(KF_ARENA_SCRATCH);

    kf_input_raw raw{};
    if (kf_input_poll(&raw) == KF_OK) {
        if (raw.quit_requested) {
            g.running = false;
        }
        debounce(raw);
    }

    /* MENU no longer toggles the HUD. It used to, and that was two separate
     * mistakes wearing one button.
     *
     * It stole a game button: MENU is how the player moves between screens
     * (ADR 0022), so every screen change also flipped a developer overlay on
     * or off, and the overlay drew on top of whatever screen you had just
     * navigated to.
     *
     * And the overlay is live timing data, so it redrew every frame -- 12% of
     * the panel, measured on hardware -- which on a display with no
     * tearing-effect signal is a visible ripple. See kf_lvgl_idempotent.h for
     * why continuous redrawing is a correctness problem here rather than a
     * performance one.
     *
     * The numbers themselves lost nothing: kf_app_log_budget_report() prints
     * them over serial once a second, the KFDBG bridge serves them as JSON on
     * demand (ADR 0030), and the desktop debug window shows them live. A
     * backend that still wants them burned into the framebuffer can call
     * kf_app_set_hud_visible() -- it is just not bound to a button the player
     * uses. */

    kf_demo_update(g.buttons_stable, g.buttons_pressed_edge);
    kf_demo_draw();

    if (g.hud_visible) {
        draw_hud();
    }

    const kf_dirty_rects dirty = kf_fb_dirty_rects();
    const size_t dirty_bytes = kf_fb_dirty_bytes();

    kf_display_present(kf_fb_pixels(), dirty.rects, dirty.count);
    kf_fb_clear_dirty();

    const uint64_t end_us = kf_time_mono_us();
    const uint32_t cpu_us = static_cast<uint32_t>(end_us - g.frame_start_us);

    const kf_display_caps *caps = kf_display_get_caps();
    /* Pixel bytes plus the per-rectangle addressing overhead each separate
     * rectangle costs on real hardware -- see KF_DISPLAY_RECT_OVERHEAD_BYTES
     * in kf/budget.h. Without this term, splitting one rectangle into many
     * would look free, which is exactly the kind of lie this estimate exists
     * to not tell. */
    const size_t overhead_bytes =
        static_cast<size_t>(dirty.count) * KF_DISPLAY_RECT_OVERHEAD_BYTES;
    const uint32_t transfer_us = estimate_transfer_us(
        dirty_bytes + overhead_bytes, caps->link_bytes_per_second);

    const kf_draw_counters counters = kf_draw_counters_get();
    const uint32_t draw_us = estimate_draw_us(counters);

    /* Reset HERE, not at the top of this function -- ADR 0036. This window
     * is NOT "this frame's drawing"; it is "everything drawn since the last
     * time this counter was read", which runs from just after the PREVIOUS
     * call's kf_draw_counters_get() above to just after THIS call's. On
     * KF_DEMO_NONE (every real device build) that window covers drawing a
     * port does AFTER kf_app_frame() returns -- kf_screen_nav_frame() on
     * ESP32/desktop, kf_creature_screen_frame() directly in
     * headless_main.cpp's run_frame_counters_check() -- because Core has no
     * way to know a port draws anything out here at all. A reset at the top
     * of this function would zero the counters again before that drawing
     * had ever been read, which is exactly why keyed_pixels/opaque_pixels
     * used to read 0 forever on KF_DEMO_NONE regardless of how expensive
     * the indexed blit actually was.
     *
     * This is deliberately the SAME window kf_fb_dirty_rects()/
     * kf_fb_clear_dirty() above already use for the same reason: the
     * creature's dirty rectangles survive kf_fb_clear_dirty() because they
     * are marked by that same out-of-band draw, and are read back exactly
     * one kf_app_frame() call later. Counters and dirty rectangles now
     * describe the same set of drawing, one frame late, on every backend --
     * not "per frame" in the sense of "this function's own execution". The
     * KF_DEMO_FULLSCREEN --stress path is unaffected: kf_demo_draw() draws
     * INSIDE this function, so its one draw and its one get()/reset() land
     * in the same call, same as before this moved. */
    kf_draw_counters_reset();

    g.last.frame_index = g.frame_index;
    g.last.cpu_us = cpu_us;
    g.last.draw_us = draw_us;
    g.last.opaque_pixels = counters.opaque_pixels;
    g.last.keyed_pixels = counters.keyed_pixels;
    g.last.transfer_us = transfer_us;
    g.last.serial_us = draw_us + transfer_us;
    g.last.overlapped_us = draw_us > transfer_us ? draw_us : transfer_us;
    g.last.total_us =
        KF_DISPLAY_DOUBLE_BUFFERED ? g.last.overlapped_us : g.last.serial_us;
    g.last.over_budget = g.last.total_us > KF_FRAME_BUDGET_US;
    g.last.dirty_percent = static_cast<uint8_t>(
        (static_cast<uint64_t>(dirty_bytes / sizeof(kf_color)) * 100ull) /
        static_cast<uint64_t>(KF_FRAMEBUFFER_PIXELS));
    g.last.dirty_rect_count = static_cast<uint8_t>(dirty.count);

    record(g.last.total_us);
    if (g.last.over_budget) {
        g.over_budget_frames++;
    }
    g.frame_index++;

    /* Report roughly once a second. Cheap, and it means the numbers are in
     * front of you constantly rather than only when you go looking. */
    if (end_us - g.last_report_us >= 1000000ull) {
        kf_app_log_budget_report();
        g.last_report_us = end_us;
    }

    /* Pace the simulator against the HOST clock, using real elapsed time
     * rather than the modelled device time. Otherwise the window would run at
     * a speed unrelated to anything. The device will replace this with a
     * light sleep, which is where the battery life comes from. */
    if (cpu_us < KF_FRAME_BUDGET_US) {
        kf_time_delay_us(KF_FRAME_BUDGET_US - cpu_us);
    }

    return g.running;
}

void kf_app_shutdown(void) {
    if (!g.initialised) {
        return;
    }
    KF_LOGI(TAG, "shutting down after %llu frames",
            static_cast<unsigned long long>(g.total_frames));
    kf_app_log_budget_report();

    kf_demo_shutdown();
    kf_assets_shutdown();
    kf_input_shutdown();
    kf_display_shutdown();
    kf_power_shutdown();
    kf_store_shutdown();
    kf_audio_shutdown();
    g.initialised = false;
    g.running = false;
}

uint32_t kf_app_buttons_held(void) { return g.buttons_stable; }
uint32_t kf_app_buttons_pressed(void) { return g.buttons_pressed_edge; }

void kf_app_debug_set_buttons(uint32_t held, uint32_t pressed_edge) {
    g.buttons_stable = held;
    g.buttons_pressed_edge = pressed_edge;
}

const kf_frame_stats *kf_app_last_frame(void) { return &g.last; }

void kf_app_set_hud_visible(bool visible) {
    if (g.hud_visible == visible) {
        return;
    }
    g.hud_visible = visible;
    /* Whichever way it just changed, something on screen needs a clean
     * repaint: turning the HUD off leaves its last frame's pixels sitting in
     * the framebuffer with nothing left to redraw them, and only the demo
     * knows its own background colour. */
    kf_demo_request_full_repaint();
}

bool kf_app_hud_visible(void) { return g.hud_visible; }

kf_frame_summary kf_app_frame_summary(void) {
    kf_frame_summary s{};
    s.frames = g.total_frames;
    s.over_budget_frames = g.over_budget_frames;
    s.last_us = g.last.total_us;
    s.worst_us = g.worst_us;

    if (g.window_count > 0) {
        uint64_t sum = 0;
        for (int i = 0; i < g.window_count; ++i) {
            sum += g.window[i];
        }
        s.mean_us = static_cast<uint32_t>(sum / static_cast<uint64_t>(g.window_count));
        s.p99_us = percentile(99);
    }
    return s;
}

void kf_app_log_budget_report(void) {
    const kf_frame_summary s = kf_app_frame_summary();
    const kf_display_caps *caps = kf_display_get_caps();

    KF_LOGI(TAG, "---- frame budget (%d fps target, %" PRIu32 " us) ----", KF_TARGET_FPS,
            static_cast<uint32_t>(KF_FRAME_BUDGET_US));
    KF_LOGI(TAG, "  device estimate: draw %5" PRIu32 " us + transfer %5" PRIu32 " us",
            g.last.draw_us, g.last.transfer_us);
    const uint32_t serial_fps_x10 = fps_x10_from_us(g.last.serial_us);
    const uint32_t overlapped_fps_x10 = fps_x10_from_us(g.last.overlapped_us);
    KF_LOGI(TAG,
            "     serial (today)      %5" PRIu32 " us  ->  %3" PRIu32
            ".%" PRIu32 " fps%s",
            g.last.serial_us, serial_fps_x10 / 10u, serial_fps_x10 % 10u,
            (!KF_DISPLAY_DOUBLE_BUFFERED && g.last.over_budget)
                ? "   OVER BUDGET"
                : "");
    KF_LOGI(TAG,
            "     overlapped (DMA+2buf) %5" PRIu32 " us  ->  %3" PRIu32
            ".%" PRIu32 " fps",
            g.last.overlapped_us, overlapped_fps_x10 / 10u,
            overlapped_fps_x10 % 10u);
    KF_LOGI(TAG, "  pixels drawn: %6" PRIu32 " opaque + %6" PRIu32 " keyed   dirty %3u%%  (%u rect%s)",
            g.last.opaque_pixels, g.last.keyed_pixels, g.last.dirty_percent,
            g.last.dirty_rect_count, g.last.dirty_rect_count == 1 ? "" : "s");
    KF_LOGI(TAG, "  host cpu %5" PRIu32 " us (your PC, not the device)", g.last.cpu_us);
    KF_LOGI(TAG, "  mean %5" PRIu32 " us   p99 %5" PRIu32 " us   worst %5" PRIu32 " us", s.mean_us,
            s.p99_us, s.worst_us);
    KF_LOGI(TAG, "  frames %llu, over budget %llu (%llu%%)",
            static_cast<unsigned long long>(s.frames),
            static_cast<unsigned long long>(s.over_budget_frames),
            s.frames ? static_cast<unsigned long long>(
                           (s.over_budget_frames * 100ull) / s.frames)
                     : 0ull);

    /* The number that most often explains a bad p99: a full-screen redraw at
     * 40MHz costs about 30ms of wire time all by itself. */
    const uint32_t full_frame_us =
        estimate_transfer_us(KF_FRAMEBUFFER_BYTES, caps->link_bytes_per_second);
    KF_LOGI(TAG, "  a FULL frame would cost %" PRIu32 " us of transfer alone",
            full_frame_us);

    KF_LOGI(TAG, "---- arenas ----");
    kf_arena_log_all();
}
