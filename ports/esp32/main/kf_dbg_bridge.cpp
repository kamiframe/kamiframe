/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_dbg_bridge.h for the shape of this (two low-priority FreeRTOS
 * tasks + two queues, command execution on the main frame-loop thread) and
 * why. This file is the ESP-IDF-specific wiring: the UART itself, the
 * tasks, the KFDBG command parser, and the command handlers -- PING, SHOT,
 * STATE, BTN, BTNHOLD, and the time-control trio ADVANCE/RESET/MULT (see
 * "Time control" below).
 *
 * Time control: an egg on this codebase's default config lasts 1 hour and
 * does not decay at all, and post-hatch decay is hours-to-days per need
 * (hunger empties in 4 real days, the slowest of the three) -- fine for a
 * shipped pet, useless for a developer watching a real device over a
 * serial link. ADVANCE/RESET mirror kf_pet_session.h's debug_advance()/
 * _reset() exactly (see that header's "DEBUG ONLY" section for what they
 * do and why they're safe to call directly -- kf_pet_advance()'s bounded-
 * loop design, ADR 0021). MULT mirrors sdl_debug_window.cpp's play-speed
 * multiplier: see handle_mult() and app_main.cpp's frame loop for how it
 * is applied.
 *
 * Wire format, confirmed against tools/kf_debug.py and tools/
 * kf_debug_selftest.py (the host side, already written and tested):
 * EVERY reply type is base64 inside the KFDBG-BEGIN/END frame, not just
 * `fb`. <length> is the base64 character count; the trailing CRC32 covers
 * the DECODED (post-base64) bytes -- an RLE stream for `fb`, plain UTF-8
 * text for `pong`/`json`/`ack`/`err`. See kf_dbg_build_reply() below,
 * which is the one function that implements this and is shared by every
 * command handler, so there is exactly one place this rule could be
 * wrong instead of five.
 *
 * Almost the entire file lives inside `#if KF_DBG_BRIDGE_ENABLE`, down to
 * the closing `#else` near the bottom that supplies the four empty
 * function bodies instead. That is deliberate, not just tidy: the first
 * cut of this file only wrapped the four public functions' BODIES in that
 * #if and left every static helper, task and global at file scope
 * unconditionally, which compiled fine but left seven unused-function/
 * unused-variable warnings the moment KF_DBG_BRIDGE_ENABLE=0 was actually
 * tried (caught by building that configuration once, not left for someone
 * else to find -- see the ADR's "Verified" section). ESP-IDF's own default
 * flags happen to exempt those particular warning classes from -Werror
 * (`-Wno-error=unused-function` etc., visible in the component's compile
 * command), so the build still succeeded -- but "zero warnings" is the
 * standard this codebase holds itself to, not "zero fatal warnings", so
 * that was a bug, not a pass. Wrapping the whole implementation is what
 * actually fixes it: with the flag off, none of this exists to be unused.
 */

#include "kf_dbg_bridge.h"

#if KF_DBG_BRIDGE_ENABLE

#include "kf_dbg_codec.h"
#include "kf_panel_profile.h"
#include "kf_pet_session.h"

#include "kf/app.h"
#include "kf/framebuffer.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *TAG = "kfdbg";

/* The same UART `idf.py monitor` already talks to -- CONFIG_ESP_CONSOLE_
 * UART_NUM, 0 on this build (see sdkconfig.defaults). Not a new pin
 * assignment: kf_esp_pins.h already reserves GPIO43/44 for it and
 * documents why nothing else may use them. */
constexpr uart_port_t kUartNum = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);

/* Generous headroom over the hardware FIFO (128 bytes on this SoC) --
 * incoming commands are a handful of bytes each, this only needs to
 * absorb bursts between kf_dbg_rx_task() reads. */
constexpr int kUartRxBufBytes = 256;

/* Longest line the parser accepts, e.g. "KFDBG BTNHOLD 4294967295
 * 4294967295" (36 chars) plus slack. A line at or past this length is
 * discarded (see kf_dbg_rx_task()), not truncated and misparsed. */
constexpr size_t kCmdMaxLen = 96;

/* Protocol-fixed: KFDBG-BEGIN/END wraps the base64 payload at this width.
 * Matches tools/kf_debug.py's own `len(line) > 76` check exactly. */
constexpr size_t kPayloadLineMax = 76;

constexpr UBaseType_t kCmdQueueDepth = 4;
/* Small on purpose: a well-behaved host (tools/kf_debug.py's own
 * SerialLink) never has more than one reply in flight -- it sends a
 * command and blocks reading the frame before sending the next. This only
 * needs to absorb the gap between kf_dbg_bridge_frame() finishing a reply
 * and kf_dbg_tx_task() picking it up, not a backlog. */
constexpr UBaseType_t kReplyQueueDepth = 2;

/* Both tasks spend nearly all their time blocked on I/O (a byte from the
 * UART, or an item from a queue), so this is generous rather than tuned --
 * there is no hardware to measure a real high-water mark against yet (see
 * the ADR's "Not verified" section). Bytes, not words: ESP-IDF's FreeRTOS
 * port takes xTaskCreate()'s stack-depth argument in bytes, unlike
 * upstream FreeRTOS. */
constexpr uint32_t kTaskStackBytes = 3072;

/* Deliberately AT MOST the default main-task priority (ESP_TASK_MAIN_PRIO,
 * 1 on this build -- see sdkconfig's CONFIG_ESP_MAIN_TASK_* block, which
 * sets everything about that task except its priority, leaving the
 * default), never above it: "low-priority" per the protocol spec means
 * these two tasks must never win a scheduling contest against the frame
 * loop. Since both spend nearly all their time blocked rather than
 * runnable, equal priority costs at most one FreeRTOS time-slice tick of
 * jitter on the rare frame where a byte or a queue item becomes ready at
 * the same instant -- not measured on real hardware, but bounded by
 * construction rather than hoped for. */
constexpr UBaseType_t kTaskPriority = tskIDLE_PRIORITY + 1;

struct KfDbgCmdMsg {
    char text[kCmdMaxLen];
};

struct KfDbgReplyMsg {
    /* heap_caps_malloc()'d by whichever kf_dbg_bridge_frame() call built
     * this reply; freed by kf_dbg_tx_task() once uart_write_bytes()
     * returns, or by kf_dbg_enqueue_reply() itself if the queue is full. */
    uint8_t *data;
    size_t len;
};

QueueHandle_t g_cmd_queue = nullptr;
QueueHandle_t g_reply_queue = nullptr;
TaskHandle_t g_rx_task = nullptr;
TaskHandle_t g_tx_task = nullptr;

/* --------------------------------------------------------------------------
 * Injected button state. Plain globals, no lock -- safe ONLY because both
 * the writer (BTN/BTNHOLD's handlers, below, called from kf_dbg_bridge_
 * frame()) and the reader (kf_dbg_input_mask(), called from esp_input.cpp's
 * kf_input_poll()) run on the same thread: app_main.cpp's loop calls
 * kf_dbg_bridge_frame() then kf_app_frame() (which calls kf_input_poll())
 * every iteration, in that order, never concurrently. If either side ever
 * moves to a different thread, this needs a lock -- it does not have one
 * today because it does not need one today, not as an oversight.
 *
 * #if KF_DBG_INPUT_INJECT_ENABLE, not just the bridge flag: a build that
 * keeps the bridge on but injection off (see kf_dbg_bridge.h) has no
 * reader or writer for this state at all -- handle_btn()/handle_btnhold()
 * skip the assignment and kf_dbg_input_mask() skips the read, both under
 * the identical #if -- so declaring it unconditionally in that
 * configuration would just be three more unused-variable warnings. -------
 */
#if KF_DBG_INPUT_INJECT_ENABLE
/* BTN: applies for exactly the next kf_input_poll() call, then clears
 * itself -- see kf_dbg_input_mask(). */
uint32_t g_inject_mask = 0;
bool g_inject_one_shot_pending = false;

/* BTNHOLD: applies until this monotonic timestamp. 0 means "not holding". */
uint64_t g_inject_hold_until_us = 0;
#endif

/* MULT: the pet-time multiplier currently in effect, read once per frame
 * by app_main.cpp's loop via kf_dbg_time_multiplier(). Same single-thread,
 * no-lock reasoning as the injected button state above -- handle_mult()
 * (below, called from kf_dbg_bridge_frame()) and kf_dbg_time_multiplier()
 * (called from app_main.cpp's loop, the same iteration, right after
 * kf_dbg_bridge_frame() returns) never run concurrently. Not under
 * #if KF_DBG_INPUT_INJECT_ENABLE like the button state above -- MULT is a
 * time-scale control, not button injection, so it stays available
 * whenever the bridge as a whole is on, the same as PING/SHOT/STATE. 1
 * means real time, unscaled -- matches sdl_debug_window.cpp's own default
 * before any multiplier button is clicked. */
uint32_t g_time_multiplier = 1;

/* --------------------------------------------------------------------------
 * The one frame builder every command handler shares. See this file's
 * header comment for the wire rule it implements: base64 the content,
 * report the base64 character count as <length>, CRC32 the RAW (pre-
 * base64) content. -------------------------------------------------------
 */

uint8_t *kf_dbg_build_reply(const char *type, const uint8_t *content,
                             size_t content_len, size_t *out_len) {
    *out_len = 0;

    const uint32_t crc = kf_dbg_crc32(content, content_len);
    const size_t b64_len = kf_dbg_base64_encoded_len(content_len);

    uint8_t *b64_buf = nullptr;
    if (b64_len > 0) {
        b64_buf = static_cast<uint8_t *>(heap_caps_malloc(b64_len, MALLOC_CAP_SPIRAM));
        if (b64_buf == nullptr) {
            KF_LOGE(TAG, "out of PSRAM encoding a %zu-char base64 payload", b64_len);
            return nullptr;
        }
        const size_t written = kf_dbg_base64_encode(
            content, content_len, reinterpret_cast<char *>(b64_buf), b64_len);
        /* kf_dbg_build_reply() always sizes b64_buf to exactly
         * kf_dbg_base64_encoded_len(content_len); a mismatch here means the
         * codec and this caller have drifted out of sync with each other,
         * not a runtime condition to degrade gracefully from. */
        KF_ASSERT(written == b64_len, "kf_dbg_build_reply: base64 length mismatch");
    }

    const size_t line_count =
        (b64_len == 0) ? 0 : ((b64_len + kPayloadLineMax - 1) / kPayloadLineMax);

    const int begin_n =
        std::snprintf(nullptr, 0, "KFDBG-BEGIN %s %zu\n", type, b64_len);
    const int end_n = std::snprintf(nullptr, 0, "KFDBG-END %08" PRIx32 "\n", crc);
    KF_ASSERT(begin_n > 0 && end_n > 0, "kf_dbg_build_reply: snprintf sizing failed");

    const size_t total = static_cast<size_t>(begin_n) + b64_len + line_count +
                          static_cast<size_t>(end_n);

    /* +1: snprintf's own trailing NUL, written one past the last real byte
     * each time it's called into this buffer below -- see the two
     * snprintf calls' own comment. */
    uint8_t *frame = static_cast<uint8_t *>(heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM));
    if (frame == nullptr) {
        KF_LOGE(TAG, "out of PSRAM for a %zu-byte `%s` frame", total, type);
        if (b64_buf != nullptr) {
            heap_caps_free(b64_buf);
        }
        return nullptr;
    }

    size_t off = 0;
    /* size = begin_n + 1: lets snprintf write its own trailing NUL at
     * frame[off + begin_n], the position the payload's first byte occupies
     * next -- safe because that position is inside the +1 headroom above
     * on the very last call, and is unconditionally overwritten by real
     * data on every earlier one. */
    off += static_cast<size_t>(std::snprintf(
        reinterpret_cast<char *>(frame + off), static_cast<size_t>(begin_n) + 1,
        "KFDBG-BEGIN %s %zu\n", type, b64_len));

    size_t remaining = b64_len;
    const uint8_t *src = b64_buf;
    while (remaining > 0) {
        const size_t chunk = remaining < kPayloadLineMax ? remaining : kPayloadLineMax;
        std::memcpy(frame + off, src, chunk);
        off += chunk;
        frame[off++] = '\n';
        src += chunk;
        remaining -= chunk;
    }

    off += static_cast<size_t>(std::snprintf(
        reinterpret_cast<char *>(frame + off), static_cast<size_t>(end_n) + 1,
        "KFDBG-END %08" PRIx32 "\n", crc));

    if (b64_buf != nullptr) {
        heap_caps_free(b64_buf);
    }

    *out_len = off;
    return frame;
}

void kf_dbg_enqueue_reply(const char *type, const uint8_t *content, size_t content_len) {
    size_t frame_len = 0;
    uint8_t *frame = kf_dbg_build_reply(type, content, content_len, &frame_len);
    if (frame == nullptr) {
        return; /* already logged inside kf_dbg_build_reply() */
    }

    KfDbgReplyMsg msg{frame, frame_len};
    if (xQueueSend(g_reply_queue, &msg, 0) != pdTRUE) {
        KF_LOGW(TAG, "reply queue full, dropping a `%s` reply (%zu bytes) -- the "
                     "previous reply (likely a SHOT) is still transmitting",
                type, frame_len);
        heap_caps_free(frame);
    }
}

void reply_err(const char *line, const char *reason) {
    char buf[128];
    const int n = std::snprintf(buf, sizeof buf, "%s: %.64s", reason, line);
    kf_dbg_enqueue_reply("err", reinterpret_cast<uint8_t *>(buf),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* --------------------------------------------------------------------------
 * Command handlers. Each builds its DECODED content (plain text, or for
 * SHOT the RLE stream) and hands it to kf_dbg_enqueue_reply(), which does
 * the base64/framing/CRC every reply type shares. ------------------------
 */

void handle_ping() {
    /* Firmware build date: __DATE__/__TIME__ expand at THIS translation
     * unit's compile time, e.g. "Aug  8 2026 14:23:01" -- the standard,
     * if slightly old-fashioned, way an embedded build stamps itself with
     * no build-system plumbing required. */
    static const char kBuildDate[] = __DATE__ " " __TIME__;

    char content[128];
    const int n = std::snprintf(content, sizeof content, "%s\n%s", kBuildDate,
                                 KF_PANEL_PROFILE.name);
    kf_dbg_enqueue_reply("pong", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_shot() {
    const size_t pixel_count = static_cast<size_t>(KF_FRAMEBUFFER_PIXELS);
    const kf_color *pixels = kf_fb_pixels();

    /* True worst case: one length-1 run per pixel. Real screens compress
     * far smaller (see the ADR's worked example) -- this is sized so
     * kf_dbg_rle_encode() can never report overflow, not because it
     * usually needs this much. PSRAM, not the internal-SRAM arenas
     * kf/budget.h manages: this is a debug facility's transient buffer,
     * not part of the constraint budget those arenas exist to enforce. */
    const size_t rle_cap = pixel_count * 4;
    uint8_t *rle_buf = static_cast<uint8_t *>(heap_caps_malloc(rle_cap, MALLOC_CAP_SPIRAM));
    if (rle_buf == nullptr) {
        KF_LOGE(TAG, "SHOT: out of PSRAM for a %zu-byte RLE worst-case buffer", rle_cap);
        reply_err("KFDBG SHOT", "out of memory encoding the framebuffer");
        return;
    }

    const size_t rle_len = kf_dbg_rle_encode(pixels, pixel_count, rle_buf, rle_cap);
    if (rle_len == static_cast<size_t>(-1)) {
        /* Unreachable given rle_cap above; kept as a hard stop rather than
         * silently sending a truncated frame if this invariant is ever
         * broken by a future change to either side. */
        heap_caps_free(rle_buf);
        KF_LOGE(TAG, "SHOT: RLE encode overflowed its own worst-case buffer");
        reply_err("KFDBG SHOT", "internal error encoding the framebuffer");
        return;
    }

    kf_dbg_enqueue_reply("fb", rle_buf, rle_len);
    heap_caps_free(rle_buf);
}

void handle_state() {
    const kf_pet_state *pet = kf_pet_session_state();
    const kf_frame_stats *last = kf_app_last_frame();
    const kf_frame_summary summary = kf_app_frame_summary();

    /* fps*10 as an integer, printed as "%lu.%lu" below -- avoids a
     * dependency on newlib's float-capable printf (not linked in by every
     * newlib configuration -- see CONFIG_LIBC_NEWLIB_NANO_FORMAT), and
     * matches this codebase's existing convention for fractional display
     * values (app_main.cpp's log_pet_state() does the identical
     * integer-tenths trick for hunger/happiness/energy percentages).
     * fps = 1e6/mean_us, so fps*10 = 1e7/mean_us. */
    const uint32_t fps_x10 = (summary.mean_us > 0)
                                  ? static_cast<uint32_t>(10000000ull / summary.mean_us)
                                  : 0u;

    const size_t heap_free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t heap_free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* pet_age_s: the pet's own lifetime clock (cumulative stage durations
     * already lived through, plus the current stage's own elapsed time) --
     * NOT the same as stage_elapsed_s above, which resets at every stage
     * transition. See kf_pet_session_debug_age_seconds()'s own comment in
     * kf_pet_session.h. Reachable here specifically because ports/esp32/
     * main/CMakeLists.txt turns KF_PET_SESSION_ENABLE_DEBUG_CONTROLS on
     * (see that file's comment) -- this is the STATE half of the same
     * time-control feature ADVANCE/RESET/MULT below make possible. */
    const uint64_t pet_age_s = kf_pet_session_debug_age_seconds();

    /* Single-line, minified JSON -- see the ADR for the exact key set;
     * field names are not fixed by the protocol spec (tools/kf_debug.py
     * prints whatever keys arrive rather than expecting specific ones), so
     * this is a firmware-side choice, documented once here and in the ADR
     * rather than duplicated in a comment at every field. */
    char json[384];
    const int n = std::snprintf(
        json, sizeof json,
        "{\"stage\":%d,\"hunger_mp\":%lu,\"happiness_mp\":%lu,\"energy_mp\":%lu,"
        "\"base_trait\":%u,\"stage_elapsed_s\":%llu,\"pet_age_s\":%llu,"
        "\"time_multiplier\":%lu,\"heap_free_internal\":%zu,"
        "\"heap_free_psram\":%zu,\"fps\":%lu.%lu,\"frame_us\":%lu}",
        static_cast<int>(pet->stage), static_cast<unsigned long>(pet->hunger_mp),
        static_cast<unsigned long>(pet->happiness_mp),
        static_cast<unsigned long>(pet->energy_mp),
        static_cast<unsigned>(pet->base_trait),
        static_cast<unsigned long long>(pet->stage_elapsed_seconds),
        static_cast<unsigned long long>(pet_age_s),
        static_cast<unsigned long>(g_time_multiplier), heap_free_internal,
        heap_free_psram, static_cast<unsigned long>(fps_x10 / 10u),
        static_cast<unsigned long>(fps_x10 % 10u),
        static_cast<unsigned long>(last->cpu_us));

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
        KF_LOGE(TAG, "STATE: JSON build failed or truncated (n=%d, cap=%zu)", n,
                sizeof(json));
        reply_err("KFDBG STATE", "internal error formatting state");
        return;
    }

    kf_dbg_enqueue_reply("json", reinterpret_cast<uint8_t *>(json),
                          static_cast<size_t>(n));
}

void handle_btn(uint32_t mask) {
    char content[64];
    int n;
#if KF_DBG_INPUT_INJECT_ENABLE
    g_inject_mask = mask;
    g_inject_one_shot_pending = true;
    g_inject_hold_until_us = 0; /* BTN supersedes any still-running BTNHOLD */
    n = std::snprintf(content, sizeof content, "BTN mask=%lu",
                       static_cast<unsigned long>(mask));
#else
    n = std::snprintf(content, sizeof content, "BTN mask=%lu (injection disabled)",
                       static_cast<unsigned long>(mask));
#endif
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_btnhold(uint32_t mask, uint32_t ms) {
    char content[64];
    int n;
#if KF_DBG_INPUT_INJECT_ENABLE
    g_inject_mask = mask;
    g_inject_one_shot_pending = false; /* BTNHOLD supersedes any pending BTN */
    g_inject_hold_until_us = kf_time_mono_us() + static_cast<uint64_t>(ms) * 1000ull;
    n = std::snprintf(content, sizeof content, "BTNHOLD mask=%lu ms=%lu",
                       static_cast<unsigned long>(mask), static_cast<unsigned long>(ms));
#else
    n = std::snprintf(content, sizeof content,
                       "BTNHOLD mask=%lu ms=%lu (injection disabled)",
                       static_cast<unsigned long>(mask), static_cast<unsigned long>(ms));
#endif
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* KFDBG ADVANCE <seconds>: kf_pet_session_debug_advance(), immediately --
 * see that function's own comment in kf_pet_session.h for why it is cheap
 * and safe to call directly (kf_pet_advance()'s bounded-loop design, ADR
 * 0021), and this file's own header comment for why a developer needs
 * this at all against this codebase's real decay rates. */
void handle_advance(uint32_t seconds) {
    kf_pet_session_debug_advance(seconds);
    char content[64];
    const int n = std::snprintf(content, sizeof content, "ADVANCE seconds=%lu",
                                 static_cast<unsigned long>(seconds));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* KFDBG RESET: kf_pet_session_debug_reset() -- a fresh egg, in place,
 * without touching whatever is currently on the device's NVS. See that
 * function's own comment in kf_pet_session.h. */
void handle_reset() {
    kf_pet_session_debug_reset();
    static const char kMsg[] = "RESET";
    kf_dbg_enqueue_reply("ack", reinterpret_cast<const uint8_t *>(kMsg),
                          sizeof(kMsg) - 1);
}

/* KFDBG MULT <n>: sets g_time_multiplier, read once per frame by
 * app_main.cpp's loop via kf_dbg_time_multiplier() and folded into ONLY
 * the delta it hands kf_pet_session_frame() -- never LVGL's tick or Lua's
 * frame delta. See kf_dbg_time_multiplier()'s own comment in
 * kf_dbg_bridge.h for the full reasoning (mirrors sdl_main.cpp's
 * identical treatment of kf_sdl_debug_window_time_multiplier() exactly).
 * Range validated by the caller (process_command_line()) against the
 * same 1..256 sdl_debug_window.cpp's multiplier buttons cover -- this
 * function is only ever called with an already-valid n. */
void handle_mult(uint32_t n) {
    g_time_multiplier = n;
    char content[32];
    const int rn = std::snprintf(content, sizeof content, "MULT n=%lu",
                                  static_cast<unsigned long>(n));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          rn > 0 ? static_cast<size_t>(rn) : 0);
}

/* --------------------------------------------------------------------------
 * Parsing. Hand-rolled rather than sscanf("KFDBG %15s", ...): a format
 * string's literal-then-%s pattern matches zero or more whitespace where
 * it looks like it requires exactly one space, so "KFDBGPING" (no space)
 * would silently parse as subcommand "PING" under sscanf. next_token()
 * below only ever consumes an actual token bounded by real whitespace, so
 * that can't happen. --------------------------------------------------- */

const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

bool next_token(const char *&p, char *out, size_t out_cap) {
    p = skip_ws(p);
    if (*p == '\0') {
        return false;
    }
    size_t n = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        if (n + 1 < out_cap) {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = '\0';
    return true;
}

bool parse_decimal(const char *tok, uint32_t *out) {
    char *endp = nullptr;
    const unsigned long v = std::strtoul(tok, &endp, 10);
    if (endp == tok || *endp != '\0') {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

void process_command_line(const char *line) {
    const char *p = line;
    char tok0[16];
    char tok1[16];

    if (!next_token(p, tok0, sizeof tok0) || std::strcmp(tok0, "KFDBG") != 0) {
        reply_err(line, "not a KFDBG command");
        return;
    }
    if (!next_token(p, tok1, sizeof tok1)) {
        reply_err(line, "missing KFDBG subcommand");
        return;
    }

    if (std::strcmp(tok1, "PING") == 0) {
        handle_ping();
    } else if (std::strcmp(tok1, "SHOT") == 0) {
        handle_shot();
    } else if (std::strcmp(tok1, "STATE") == 0) {
        handle_state();
    } else if (std::strcmp(tok1, "BTN") == 0) {
        char tok2[16];
        uint32_t mask = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &mask)) {
            reply_err(line, "KFDBG BTN needs one decimal mask argument");
            return;
        }
        handle_btn(mask);
    } else if (std::strcmp(tok1, "BTNHOLD") == 0) {
        char tok2[16];
        char tok3[16];
        uint32_t mask = 0;
        uint32_t ms = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &mask) ||
            !next_token(p, tok3, sizeof tok3) || !parse_decimal(tok3, &ms)) {
            reply_err(line, "KFDBG BTNHOLD needs decimal mask and ms arguments");
            return;
        }
        handle_btnhold(mask, ms);
    } else if (std::strcmp(tok1, "ADVANCE") == 0) {
        char tok2[16];
        uint32_t seconds = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &seconds)) {
            reply_err(line, "KFDBG ADVANCE needs one decimal seconds argument");
            return;
        }
        handle_advance(seconds);
    } else if (std::strcmp(tok1, "RESET") == 0) {
        handle_reset();
    } else if (std::strcmp(tok1, "MULT") == 0) {
        char tok2[16];
        uint32_t mult = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &mult)) {
            reply_err(line, "KFDBG MULT needs one decimal multiplier argument");
            return;
        }
        /* 1..256: same range sdl_debug_window.cpp's multiplier buttons
         * cover (1x through 256x) -- see kf_dbg_time_multiplier()'s own
         * comment in kf_dbg_bridge.h. Rejected here, before handle_mult()
         * ever runs, so g_time_multiplier is never set to anything
         * outside that range. */
        if (mult < 1u || mult > 256u) {
            reply_err(line, "KFDBG MULT must be between 1 and 256");
            return;
        }
        handle_mult(mult);
    } else {
        reply_err(line, "unknown KFDBG subcommand");
    }
}

/* --------------------------------------------------------------------------
 * The two tasks. Both spend nearly all their time blocked -- see
 * kf_dbg_bridge.h's "WHY A BACKGROUND TASK" comment for why that is the
 * point, not a missed optimisation. -------------------------------------
 */

void kf_dbg_rx_task(void * /*arg*/) {
    char line[kCmdMaxLen];
    size_t line_len = 0;
    bool discarding = false; /* true while resyncing past an overlong line */

    for (;;) {
        uint8_t byte = 0;
        /* One byte per call: simple, and cheap enough that it doesn't
         * matter -- this task is not on any critical path, and every byte
         * it reads was already sitting in the UART driver's own ring
         * buffer (filled by its ISR), not fetched from hardware here. */
        const int n = uart_read_bytes(kUartNum, &byte, 1, portMAX_DELAY);
        if (n <= 0) {
            continue;
        }

        if (byte == '\n') {
            if (discarding) {
                discarding = false;
                line_len = 0;
                continue;
            }
            if (line_len == 0) {
                continue; /* a bare blank line: not worth an err reply */
            }
            KfDbgCmdMsg msg;
            std::memcpy(msg.text, line, line_len);
            msg.text[line_len] = '\0';
            if (xQueueSend(g_cmd_queue, &msg, 0) != pdTRUE) {
                KF_LOGW(TAG, "command queue full, dropping: %s", msg.text);
            }
            line_len = 0;
            continue;
        }

        if (discarding) {
            continue; /* swallow until the next newline resyncs us */
        }

        if (byte == '\r') {
            continue; /* CRLF: the \n above ends the line either way */
        }

        if (line_len + 1 >= sizeof(line)) {
            KF_LOGW(TAG, "KFDBG command line exceeded %zu bytes, discarding it",
                    sizeof(line));
            discarding = true;
            line_len = 0;
            continue;
        }

        line[line_len++] = static_cast<char>(byte);
    }
}

/* ---------------------------------------------------------------------------
 * Console muting during a transfer.
 *
 * This is a CORRECTNESS requirement, not tidiness, and it was found the
 * expensive way -- on hardware, after the first real screenshot failed twice.
 *
 * The bridge shares one UART with every KF_LOG/ESP_LOG line the firmware
 * emits, and those come from other tasks. A screenshot takes seconds to
 * stream while the frame budget report prints once a second, so a log WILL
 * arrive mid-transfer. The damaging part is that two writers to one UART do
 * not interleave at line boundaries: the log's bytes land INSIDE a base64
 * payload line, splicing text into the middle of it.
 *
 * That cannot be repaired on the host. The injected log text is made of
 * letters and digits which are themselves valid base64 characters, so a
 * parser cannot tell payload from intruder. Both host-side attempts proved
 * it: counting the merged line gave 45 characters too many, rejecting it gave
 * 38 too few, and neither number is recoverable.
 *
 * So the device stops talking while it transmits. Log lines emitted during a
 * transfer are dropped, not queued -- a few missing lines during a debug
 * screenshot is a trade worth making, and buffering them would need memory
 * proportional to how long the transfer takes.
 *
 * One honest limitation: a task already inside the log function when the mute
 * is set can still get its bytes out. That window is microseconds against a
 * multi-second transfer, and the CRC32 catches the result, so the failure
 * mode is a clear error and a retry rather than a silently corrupt image.
 * ------------------------------------------------------------------------- */
volatile bool g_console_muted = false;
vprintf_like_t g_prev_vprintf = nullptr;

int kf_dbg_log_filter(const char *fmt, va_list args) {
    if (g_console_muted) {
        return 0;
    }
    if (g_prev_vprintf != nullptr) {
        return g_prev_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

void kf_dbg_tx_task(void * /*arg*/) {
    for (;;) {
        KfDbgReplyMsg msg{};
        if (xQueueReceive(g_reply_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.data != nullptr) {
            if (msg.len > 0) {
                /* tx_buffer_size 0 (see kf_dbg_bridge_init()) means this
                 * blocks until every byte has cleared the FIFO -- exactly
                 * what should happen here, on this dedicated task, and
                 * exactly what must NOT happen on the frame-loop thread
                 * (see kf_dbg_bridge.h's header comment). */
                g_console_muted = true;
                uart_write_bytes(kUartNum, msg.data, msg.len);
                g_console_muted = false;
            }
            heap_caps_free(msg.data);
        }
    }
}

} // namespace

void kf_dbg_bridge_init(void) {
    g_cmd_queue = xQueueCreate(kCmdQueueDepth, sizeof(KfDbgCmdMsg));
    g_reply_queue = xQueueCreate(kReplyQueueDepth, sizeof(KfDbgReplyMsg));
    KF_ASSERT(g_cmd_queue != nullptr && g_reply_queue != nullptr,
              "kf_dbg_bridge_init: queue creation failed");

    /* Deliberately no uart_param_config()/uart_set_pin() call: the
     * console's own startup already configured this UART's baud rate and
     * pins (CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200, GPIO43/44 -- see
     * kf_esp_pins.h) before app_main() ever ran. Re-configuring here would
     * risk fighting that rather than sharing it. tx_buffer_size 0: TX is
     * unbuffered by the driver, so kf_dbg_tx_task()'s uart_write_bytes()
     * blocks on FIFO space directly -- fine, since that call only ever
     * happens on that dedicated task (see its own comment). queue_size 0:
     * this reads bytes directly, no event queue needed. */
    const esp_err_t err = uart_driver_install(kUartNum, kUartRxBufBytes, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "uart_driver_install failed (%d) -- KFDBG bridge is disabled "
                     "this session",
                static_cast<int>(err));
        return;
    }

    xTaskCreate(kf_dbg_rx_task, "kf_dbg_rx", kTaskStackBytes, nullptr, kTaskPriority,
                &g_rx_task);
    /* Route every log line through the mute filter -- see its comment above.
     * esp_log_set_vprintf() catches KF_LOG (which goes through esp_log_write)
     * and ESP-IDF's own ESP_LOGx alike, which is why it is done here rather
     * than by teaching esp_log.cpp about the bridge. Bare printf() bypasses
     * it, but the only bare printfs are the boot banner, long before any
     * transfer. */
    g_prev_vprintf = esp_log_set_vprintf(kf_dbg_log_filter);

    xTaskCreate(kf_dbg_tx_task, "kf_dbg_tx", kTaskStackBytes, nullptr, kTaskPriority,
                &g_tx_task);

    KF_LOGI(TAG, "KFDBG bridge up on UART%d (console baud), input inject %s",
            static_cast<int>(kUartNum), KF_DBG_INPUT_INJECT_ENABLE ? "ON" : "OFF");
}

void kf_dbg_bridge_frame(void) {
    KfDbgCmdMsg msg;
    if (xQueueReceive(g_cmd_queue, &msg, 0) == pdTRUE) {
        process_command_line(msg.text);
    }
}

void kf_dbg_bridge_shutdown(void) {
    /* Unreachable in practice -- see kf_dbg_bridge.h's own comment, the
     * same "correct shape, dead code" reasoning app_main.cpp's own
     * shutdown block uses. No attempt is made to drain in-flight queue
     * items first; a real shutdown-capable backend would need to. */
    if (g_rx_task != nullptr) {
        vTaskDelete(g_rx_task);
        g_rx_task = nullptr;
    }
    if (g_prev_vprintf != nullptr) {
        esp_log_set_vprintf(g_prev_vprintf);
        g_prev_vprintf = nullptr;
    }
    g_console_muted = false;
    if (g_tx_task != nullptr) {
        vTaskDelete(g_tx_task);
        g_tx_task = nullptr;
    }
    uart_driver_delete(kUartNum);
    if (g_cmd_queue != nullptr) {
        vQueueDelete(g_cmd_queue);
        g_cmd_queue = nullptr;
    }
    if (g_reply_queue != nullptr) {
        vQueueDelete(g_reply_queue);
        g_reply_queue = nullptr;
    }
}

uint32_t kf_dbg_input_mask(void) {
#if KF_DBG_INPUT_INJECT_ENABLE
    if (g_inject_one_shot_pending) {
        g_inject_one_shot_pending = false;
        const uint32_t m = g_inject_mask;
        g_inject_mask = 0;
        return m;
    }
    if (g_inject_hold_until_us != 0) {
        if (kf_time_mono_us() < g_inject_hold_until_us) {
            return g_inject_mask;
        }
        g_inject_hold_until_us = 0;
        g_inject_mask = 0;
    }
    return 0;
#else
    return 0;
#endif
}

uint32_t kf_dbg_time_multiplier(void) { return g_time_multiplier; }

#else // !KF_DBG_BRIDGE_ENABLE

/* The whole bridge, compiled away: no task, no queue, no UART driver
 * install, nothing -- see kf_dbg_bridge.h's own comment on this flag.
 * kf_dbg_input_mask() returns 0 unconditionally, so esp_input.cpp's
 * `mask |= kf_dbg_input_mask();` is always a no-op OR, needing no #if of
 * its own at the call site. kf_dbg_time_multiplier() returns 1 (real
 * time, unscaled), not 0 -- there is no MULT command left to change it,
 * and app_main.cpp's loop multiplies this straight into the pet session's
 * delta, so 1 is "behave exactly as if this feature did not exist",
 * which is precisely what disabling the bridge should mean. */

void kf_dbg_bridge_init(void) {}
void kf_dbg_bridge_frame(void) {}
void kf_dbg_bridge_shutdown(void) {}
uint32_t kf_dbg_input_mask(void) { return 0; }
uint32_t kf_dbg_time_multiplier(void) { return 1; }

#endif // KF_DBG_BRIDGE_ENABLE
