/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 * See kf_dbg_bridge.h for the shape of this (two low-priority FreeRTOS
 * tasks + two queues, command execution on the main frame-loop thread) and
 * why. This file is the ESP-IDF-specific wiring: the UART itself, the
 * tasks, the KFDBG command parser, and the command handlers -- PING, SHOT,
 * STATE, SCANLINE, VSYNC, RTC (see "RTC" below), BTN, BTNHOLD, the
 * time-control quartet ADVANCE/RESET/MULT/CLOCK (see "Time control"
 * below), the five care actions FEED/PLAY/REST/BATH/FLUSH, and JUMP (see
 * "Care actions and stage jump" below).
 * SCANLINE: samples the ILI9341's Get_Scanline register (command 0x45) 64
 * times in a row over SPI and reports the pattern, so a human can judge
 * whether beam-racing -- delaying a write until the panel's own scan-out has
 * passed the rectangle about to be touched -- is worth building on this
 * module. This board's 18-pin display flex has no tearing-effect (TE) pin,
 * confirmed against the manufacturer's schematic, so polling this register
 * is the only remaining way to find out. handle_scanline() below is
 * measurement only -- it does not wait for, or act on, a scanline value
 * anywhere. It reads at a genuinely slow clock (2MHz by default, `KFDBG
 * SCANLINE <read_hz>` to override), NOT the 40MHz write clock every frame
 * uses -- the ILI9341's read cycle is only rated to about 6MHz, and reading
 * that much too fast is the confirmed, already-measured explanation for why
 * the first attempt at this diagnostic came back looking like noise. Getting
 * a slower clock means tearing down and rebuilding the one esp_lcd panel IO
 * handle around the sampling loop and rebuilding it back afterwards -- see
 * kf_esp_display_diag.h for the raw primitive and the begin/end-probe pair
 * this handler drives, and esp_display.cpp's kf_esp_display_diag_begin_
 * probe() for why that rebuild, rather than a second SPI device, is the safe
 * way to get a different clock out of esp_lcd. A clean 1/2/4MHz sweep has
 * since confirmed the ALT framing (no dummy byte) as correct for this panel
 * -- handle_scanline() below now reports that framing under the unprefixed
 * fields (value/distinct_values/increases/decreases/...) and the OLD
 * datasheet framing (1 dummy byte) under the alt_-prefixed ones, the reverse
 * of this diagnostic's first cut. dummy_bytes_assumed in the JSON reply
 * always describes the unprefixed fields' framing, so it changed from 1 to
 * 0 along with the swap.
 * VSYNC: the feature this diagnostic exists to justify. `KFDBG VSYNC <0|1>`
 * toggles esp_display.cpp's push_rect() waiting for the scan to clear a
 * rectangle before writing it -- see that file's own header comment on the
 * feature (above push_rect()) for the read-at-40MHz reasoning and the wait
 * itself, and kf_esp_display_vsync.h for the on/off and stats contract this
 * file's handle_vsync() and handle_state() are the two ends of.
 * Time control: an egg on this codebase's default config lasts 1 hour and
 * does not decay at all, and post-hatch decay is hours-to-days per need
 * (hunger empties in 4 real days, the slowest of the three) -- fine for a
 * shipped pet, useless for a developer watching a real device over a
 * serial link. ADVANCE/RESET mirror kf_pet_session.h's debug_advance()/
 * _reset() exactly (see that header's "DEBUG ONLY" section for what they
 * do and why they're safe to call directly -- kf_pet_advance()'s bounded-
 * loop design, ADR 0021). MULT mirrors sdl_debug_window.cpp's play-speed
 * multiplier: see handle_mult() and app_main.cpp's frame loop for how it
 * is applied. CLOCK mirrors sdl_debug_window.cpp's Drowsy/Bedtime/Morning
 * buttons (KFDBG CLOCK DROWSY/BEDTIME/MORNING, via
 * kf_pet_session_debug_clock_target() then kf_pet_session_debug_set_clock()
 * -- see handle_clock_point(), and kf_pet_session.h's own comment on why
 * BOTH clocks have to move together) plus one thing the desktop window
 * doesn't offer at all: KFDBG CLOCK EPOCH <seconds> sets the world clock to
 * an arbitrary epoch, which is what makes a drifted real-time-clock DATE on
 * a real board fixable over this bridge (see handle_clock_epoch(), and
 * ports/esp32/README.md's former "no way to set the date from the device"
 * open question -- this closes it).
 * RTC: KFDBG RTC reads the DS3231 real-time-clock chip's registers DIRECTLY
 * over I2C, not kf_time_wall() (see kf_esp_time_debug.h's own comment on
 * why that distinction is the entire point) -- an observe-tier command,
 * gated by KF_DBG_BRIDGE_ENABLE alone like PING/SHOT/STATE/ SCANLINE/VSYNC,
 * never by KF_DBG_MUTATE_ENABLE, because reading a chip's registers changes
 * nothing. Built for Task 5 of the screens/clock/sleep plan's bench
 * procedure: comparing this against KFDBG STATE's implicit reliance on the
 * RAM clock is what proves (or disproves) that the two haven't drifted
 * apart, and is the only way to observe the chip's OSF (oscillator-stopped)
 * flag from off-device at all -- the coin-cell-removed negative case that
 * plan explicitly calls "not optional" has no other way to be checked
 * remotely.
 * Care actions and stage jump: the desktop simulator reaches all five care
 * actions (feed/play/rest/bath/flush) off number keys 1-5 (sdl_input.cpp)
 * and a life-stage jump off sdl_debug_window.cpp's own buttons -- neither
 * had a KFDBG equivalent until this file's FEED/PLAY/REST/BATH/FLUSH/JUMP
 * handlers below. All six call kf_pet_session_feed()/_play()/_rest()/
 * _bath()/_flush()/kf_pet_session_debug_jump_to_stage() DIRECTLY, not via
 * KFDBG BTN's button-injection path, even though a real button press
 * reaches the four care functions through kf_home_screen_input.cpp's
 * kf_home_screen_handle_care_buttons(). See handle_feed()'s own comment
 * for the two
 * reasons (explicit variation vs. hidden per-button
 * cycling state; sidestepping Core's debounce, which a one-shot BTN
 * injection cannot reliably clear -- see tools/kf_debug.py's `press`
 * --hold-ms comment for that specific, already-found bug).
 * FEED/PLAY/REST/BATH/FLUSH/JUMP/ADVANCE/RESET/MULT/CLOCK/BTN/BTNHOLD --
 * every command that changes the pet or the simulation, TWELVE in all
 * since CLOCK joined the set -- are gated behind KF_DBG_MUTATE_ENABLE (see
 * kf_dbg_bridge.h and ADR 0035, which supersedes the paragraph this
 * replaced; CLOCK rides the identical gate for the identical reason: it
 * changes the pet's own notion of what time it is, which is exactly the
 * "time and care are real" premise this flag exists to protect). The nine
 * ADR 0035 originally covered that are not button injection (ADVANCE/
 * RESET/MULT/FEED/PLAY/REST/BATH/FLUSH/JUMP) had no flag narrower than the
 * whole-bridge switch before that ADR -- BTN/BTNHOLD alone were gated, by
 * KF_DBG_INPUT_INJECT_ENABLE, on the reasoning that only button injection
 * was "remote control". That boundary meant switching off button injection
 * alone gave false assurance -- a serial cable could still refill a
 * neglected pet's needs or jump it straight to adult, the ultimate cheat
 * for a pet whose premise is that time and care are real. require_mutate_
 * enabled(), called first thing in each of these twelve branches below
 * (BTN/BTNHOLD included, nested one level deeper under this flag rather
 * than losing their own narrower gate), is the single choke point that
 * enforces the split -- see its own comment. PING/SHOT/STATE/SCANLINE/
 * VSYNC/RTC stay gated by KF_DBG_BRIDGE_ENABLE alone, same as always: none
 * of them changes anything. Turning off KF_DBG_BRIDGE_ENABLE entirely
 * still removes all of it, same as everything else in this file.
 * Wire format, confirmed against tools/kf_debug.py and tools/
 * kf_debug_selftest.py (the host side, already written and tested):
 * EVERY reply type is base64 inside the KFDBG-BEGIN/END frame, not just
 * `fb`. <length> is the base64 character count; the trailing CRC32 covers
 * the DECODED (post-base64) bytes -- an RLE stream for `fb`, plain UTF-8
 * text for `pong`/`json`/`ack`/`err`. See kf_dbg_build_reply() below,
 * which is the one function that implements this and is shared by every
 * command handler, so there is exactly one place this rule could be
 * wrong instead of five.
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

#include "kf_app_post_frame.h"
#include "kf_dbg_codec.h"
#include "kf_esp_display_diag.h"
#include "kf_esp_display_vsync.h"
#include "kf_esp_time_debug.h"
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
 * #if KF_DBG_INPUT_INJECT_ENABLE, not just the bridge flag (or, since
 * ADR 0035, the mutate flag it now nests inside -- see kf_dbg_bridge.h): a
 * build that keeps mutation on but injection off has no reader or writer
 * for this state at all -- handle_btn()/handle_btnhold() skip the
 * assignment and kf_dbg_input_mask() skips the read, both under the
 * identical #if -- so declaring it unconditionally in that configuration
 * would just be three more unused-variable warnings. -------------------
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
 * time-scale control, not button injection, so it is never narrowed by
 * that flag specifically. It IS gated by KF_DBG_MUTATE_ENABLE, same as
 * every other command that changes the pet or the simulation (ADR 0035):
 * require_mutate_enabled(), checked first thing in KFDBG MULT's dispatch
 * branch below, is what actually keeps g_time_multiplier at its default
 * when mutation is off -- unlike PING/SHOT/STATE, which stay available
 * whenever just the bridge is on. 1 means real time, unscaled -- matches
 * sdl_debug_window.cpp's own default before any multiplier button is
 * clicked. */
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

/* Plain compile-time constant, not a mutable global: KF_DBG_MUTATE_ENABLE
 * is always literally 0 or 1, so `if (!kMutateEnabled)` below is a branch
 * on a value known at compile time, not on state that changes at runtime.
 * That does NOT mean the twelve handle_*() bodies this flag guards vanish
 * from the binary the way KF_DBG_BRIDGE_ENABLE=0 makes this entire file
 * vanish (see that flag's own #if near the top) -- confirmed by `nm`
 * against a real KF_DBG_MUTATE_ENABLE=0 esp32s3 build (ADR 0035's
 * Verified section, for the eleven that existed at that time; CLOCK is new
 * and rides the identical, already-verified mechanism, not a fresh one):
 * handle_feed()/handle_advance()/handle_clock_point()/etc. are all still
 * present as local symbols in the compiled object file, because
 * process_command_line()'s twelve-way if/else chain is one large function
 * and the compiler does not fold the constant far enough to prove each
 * individual handler call unreachable at this optimization level.
 * Functionally identical either way -- every one of those calls is
 * unconditionally preceded by `if (!require_mutate_enabled(line)) return;`
 * below, so none of them ever runs with the flag off -- but "unreachable,
 * still linked in" is the honest description here, not "removed". That is
 * the same shape KF_DBG_INPUT_INJECT_ENABLE=0 already has for BTN/
 * BTNHOLD (handle_btn()/handle_btnhold() themselves stay linked at that
 * flag's off setting too; only their #if/#else-swapped bodies change --
 * see those functions below), which is the precedent this flag follows
 * rather than reinventing: a single runtime check on a compile-time
 * constant, not a second family of #if/#else bodies. See kf_dbg_bridge.h's
 * own comment on KF_DBG_MUTATE_ENABLE for what it gates and why it is a
 * separate flag from KF_DBG_BRIDGE_ENABLE. */
constexpr bool kMutateEnabled = KF_DBG_MUTATE_ENABLE;

/* Guards every KFDBG command that mutates the pet or the simulation --
 * FEED/PLAY/REST/BATH/FLUSH/JUMP/ADVANCE/RESET/MULT/CLOCK/BTN/BTNHOLD --
 * called as the first line of each of those twelve branches in
 * process_command_line() below: `if (!require_mutate_enabled(line)) {
 * return; }`. Returns true (and does nothing else) when mutation is
 * allowed; otherwise replies `err` naming the exact flag to flip and
 * returns false, so the caller can bail before doing any of that command's
 * real work or even finishing its own argument parsing.
 *
 * The `err` reply, not a dropped command or a bare protocol error, is the
 * point (ADR 0035's "host side" section): tools/kf_debug.py's own
 * `_expect()` already turns any `err` reply into `KfDebugError(f"device
 * rejected \`{command}\`: {payload}")`, so whatever text reply_err() sends
 * here is what a developer actually reads on the host -- naming
 * KF_DBG_MUTATE_ENABLE explicitly is what makes that message actionable
 * instead of merely accurate. Matches every other inline validation in
 * this file (parse_care_variation()'s own reply_err() calls, the range
 * checks in process_command_line() below) in shape, just checked before
 * any of them rather than after. */
bool require_mutate_enabled(const char *line) {
    if (kMutateEnabled) {
        return true;
    }
    reply_err(line, "mutating KFDBG commands are disabled on this build "
                     "-- set KF_DBG_MUTATE_ENABLE=1 to re-enable (PING/"
                     "SHOT/STATE/SCANLINE/VSYNC still work)");
    return false;
}

/* Guards SCANLINE and VSYNC, the two commands that only mean anything on a
 * panel profile with a physical read line (kf_panel_profile.h's
 * has_read_line, ADR 0039) -- called as the first line of handle_scanline()
 * and handle_vsync() below, the same "check first, bail before doing any
 * real work" shape require_mutate_enabled() above uses.
 *
 * Not a compile-time #if: has_read_line is a property of KF_PANEL_PROFILE,
 * which is resolved at compile time, but reading it through
 * kf_esp_display_has_read_line() rather than #ifdef-ing this file against a
 * specific profile keeps kf_dbg_bridge.cpp panel-agnostic, matching every
 * other file in this port that goes through kf_panel_profile.h's data
 * rather than special-casing a controller.
 *
 * `label` is the command name to echo in the reply, e.g. "KFDBG SCANLINE" --
 * neither caller has the raw command `line` in scope (both take already-
 * parsed arguments), so this takes a fixed label the same way
 * handle_scanline()'s own out-of-memory reply_err() calls already do. */
bool require_read_line(const char *label) {
    if (kf_esp_display_has_read_line()) {
        return true;
    }
    char reason[96];
    std::snprintf(reason, sizeof reason,
                  "no read line on this panel (%s) -- nothing to read",
                  kf_esp_display_panel_name());
    reply_err(label, reason);
    return false;
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

    /* vsync_*: the beam-racing feature's own runtime setting and its most
     * recently completed one-second window of stats -- see
     * kf_esp_display_vsync.h's own comment on kf_esp_display_vsync_get_
     * stats() for exactly what "most recently completed" means (it does not
     * decay to zero during an idle pet). Reading these costs nothing beyond
     * three uint32_t copies out of esp_display.cpp's own file-scope state --
     * no lock needed, same single-thread reasoning as everything else this
     * function reads. */
    const bool vsync_enabled = kf_esp_display_vsync_enabled();
    uint32_t vsync_rects_written = 0;
    uint32_t vsync_rects_waited = 0;
    uint32_t vsync_avg_wait_us = 0;
    kf_esp_display_vsync_get_stats(&vsync_rects_written, &vsync_rects_waited,
                                    &vsync_avg_wait_us);

    /* post_us: the segment Core cannot see or time itself -- everything a
     * PORT draws after kf_app_frame() returns (the creature, on this
     * build). See kf_app_post_frame.h and app_main.cpp's own comment on
     * where this is measured. ADR 0036 is the reason it exists at all:
     * before that task, there was no wire field carrying ANY of the budget
     * numbers below except fps/frame_us, and keyed_px would have read 0 on
     * every device regardless of what this line reports. */
    const uint32_t post_us = kf_app_post_frame_us();

    /* asleep/drowsy/tucked_in: the sleep state (ADR 0048/0052) the desktop
     * debug window has always had in-process access to (it reads
     * kf_pet_session_state() directly) but the hardware bridge never
     * carried -- see this file's own header comment on why that gap
     * mattered: the panel had no way to show sleep state at all. asleep and
     * tucked_in are plain kf_pet_state fields; drowsy is derived, same as
     * every other reader of it (kf_pet_drowsy(), hakoniwaos/include/kf/
     * pet.h -- NOT a separate saved sub-state, see that function's own
     * comment). */
    const bool drowsy = kf_pet_drowsy(pet);

    /* Single-line, minified JSON -- see the ADR for the exact key set;
     * field names are not fixed by the protocol spec (tools/kf_debug.py
     * prints whatever keys arrive rather than expecting specific ones), so
     * this is a firmware-side choice, documented once here and in the ADR
     * rather than duplicated in a comment at every field.
     *
     * cpu_us duplicates frame_us's value under ADR 0036's new name --
     * frame_us stays exactly as it was (existing hosts and habits depend on
     * it, per this task's own brief), cpu_us is the name Tasks 5-8 and
     * tools/kf_debug.py's new budget line actually read, matching kf/
     * app.h's kf_frame_stats::cpu_us field name instead of this file's
     * older, unrelated one.
     *
     * 1024, recomputed for asleep/drowsy/tucked_in (ADR 0054): the literal
     * text of the format string below (every key name, quote, colon and
     * comma, no substitutions) is 463 bytes; the worst-case width of every
     * substituted value -- uint64_t fields (%llu, 4 of them) as 20 digits,
     * every uint32_t/size_t/%d field (24 of them) as 10 digits, and the
     * five %s fields (vsync_enabled, over_budget, and the three new
     * booleans) as "false" (5) -- sums to 4*20 + 24*10 + 5*5 = 345; plus
     * the NUL snprintf always writes, that is 809 in the worst case.
     * (Derivation checked by exhaustively enumerating every specifier and
     * its cast in the snprintf call below, not by re-deriving the OLD
     * comment's 773 -- that number no longer matches a literal+specifier
     * count of the pre-Task-5.5 format string either, so it is not trusted
     * as a baseline here; 809 is verified against THIS format string as
     * written.) 1024 clears 809 with 215 bytes of margin -- still enough
     * for a future field or two before this buffer is the next thing that
     * needs raising. */
    char json[1024];
    const int n = std::snprintf(
        json, sizeof json,
        "{\"stage\":%d,\"hunger_mp\":%lu,\"happiness_mp\":%lu,\"energy_mp\":%lu,"
        "\"base_trait\":%u,\"stage_elapsed_s\":%llu,\"pet_age_s\":%llu,"
        "\"time_multiplier\":%lu,\"heap_free_internal\":%zu,"
        "\"heap_free_psram\":%zu,\"fps\":%lu.%lu,\"frame_us\":%lu,"
        "\"vsync_enabled\":%s,\"vsync_rects_written\":%lu,"
        "\"vsync_rects_waited\":%lu,\"vsync_avg_wait_us\":%lu,"
        "\"draw_us\":%lu,\"transfer_us\":%lu,\"cpu_us\":%lu,\"post_us\":%lu,"
        "\"dirty_rects\":%u,\"dirty_pct\":%u,\"opaque_px\":%lu,"
        "\"keyed_px\":%lu,\"over_budget\":%s,\"worst_us\":%lu,"
        "\"p99_us\":%lu,\"frames\":%llu,\"over_budget_frames\":%llu,"
        "\"asleep\":%s,\"drowsy\":%s,\"tucked_in\":%s}",
        static_cast<int>(pet->stage), static_cast<unsigned long>(pet->hunger_mp),
        static_cast<unsigned long>(pet->happiness_mp),
        static_cast<unsigned long>(pet->energy_mp),
        static_cast<unsigned>(pet->base_trait),
        static_cast<unsigned long long>(pet->stage_elapsed_seconds),
        static_cast<unsigned long long>(pet_age_s),
        static_cast<unsigned long>(g_time_multiplier), heap_free_internal,
        heap_free_psram, static_cast<unsigned long>(fps_x10 / 10u),
        static_cast<unsigned long>(fps_x10 % 10u),
        static_cast<unsigned long>(last->cpu_us),
        vsync_enabled ? "true" : "false",
        static_cast<unsigned long>(vsync_rects_written),
        static_cast<unsigned long>(vsync_rects_waited),
        static_cast<unsigned long>(vsync_avg_wait_us),
        static_cast<unsigned long>(last->draw_us),
        static_cast<unsigned long>(last->transfer_us),
        static_cast<unsigned long>(last->cpu_us),
        static_cast<unsigned long>(post_us),
        static_cast<unsigned>(last->dirty_rect_count),
        static_cast<unsigned>(last->dirty_percent),
        static_cast<unsigned long>(last->opaque_pixels),
        static_cast<unsigned long>(last->keyed_pixels),
        last->over_budget ? "true" : "false",
        static_cast<unsigned long>(summary.worst_us),
        static_cast<unsigned long>(summary.p99_us),
        static_cast<unsigned long long>(summary.frames),
        static_cast<unsigned long long>(summary.over_budget_frames),
        pet->asleep ? "true" : "false", drowsy ? "true" : "false",
        pet->tucked_in ? "true" : "false");

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
        KF_LOGE(TAG, "STATE: JSON build failed or truncated (n=%d, cap=%zu)", n,
                sizeof(json));
        reply_err("KFDBG STATE", "internal error formatting state");
        return;
    }

    kf_dbg_enqueue_reply("json", reinterpret_cast<uint8_t *>(json),
                          static_cast<size_t>(n));
}

/* KFDBG SCANLINE -- see this file's own header comment for what this is
 * investigating and why. Every allocation here is PSRAM, not stack, same
 * discipline handle_shot() already uses for its own bulk buffer: this runs
 * on the main frame-loop thread, sharing its one ~3.5KB stack
 * (CONFIG_ESP_MAIN_TASK_STACK_SIZE) with kf_app_frame(), LVGL and Lua, and
 * 64 samples plus a JSON reply is enough bytes that putting it there rather
 * than on the heap would be the kind of thing that works in testing and
 * overflows the day something else on that stack gets a little deeper. */
constexpr int kScanlineSampleCount = 64;
constexpr size_t kScanlineReplyBytes = 3; /* 1 assumed dummy byte + 2 data bytes -- see kf_esp_display_diag.h */
constexpr int kScanlineReportStride = 5;  /* every 5th sample reported, not all 64 -- representative, keeps the reply small */

/* Default read clock for `KFDBG SCANLINE` with no argument. The ILI9341's
 * read cycle is roughly 150ns per the datasheet -- about 6MHz max -- and
 * the write clock this bus normally runs at (KF_DISPLAY_SPI_HZ, 40MHz) is
 * 6-20x past that: the concrete, already-measured explanation for why the
 * first SCANLINE run (at the write clock) came back as runs of all-zero and
 * all-one bytes jumping by exactly 127, the classic signature of sampling
 * MISO too fast rather than of a real scan counter. 2MHz sits comfortably
 * under the ~6MHz ceiling. `KFDBG SCANLINE <read_hz>` (see
 * process_command_line() below) overrides this from the host, so a human
 * can try 1MHz or 4MHz without a firmware rebuild -- see
 * tools/kf_debug.py's `scanline --read-hz`. */
constexpr uint32_t kScanlineDefaultReadHz = 2000000;

struct ScanlineSample {
    /* value: the framing a clean 1/2/4MHz sweep confirmed -- NO dummy byte,
     * raw[0]/raw[1] read MSB-first as the 10-bit value. This is now
     * "primary" in every sense that matters here: it is what value_min/max,
     * distinct_values, increases/decreases and changed_between_reads (the
     * unprefixed fields in the JSON reply) are computed from, and
     * dummy_bytes_assumed in that reply is 0, describing this field.
     *
     * alt_value: the OLD hypothesis this diagnostic originally led with --
     * the ILI9341 datasheet's documented framing, one dummy byte (raw[0])
     * then a 10-bit value MSB-first across raw[1]/raw[2]. Confirmed WRONG
     * for this panel by the same sweep (its byte-boundary artefacts --
     * runs of 0x00/0x80/0xFF, jumping by ~127 -- never looked like a scan
     * counter at any of the three clocks tried), but still computed and
     * reported here, under the alt_-prefixed JSON fields, in case a future
     * panel profile turns out to need it instead -- see kf_esp_display_
     * diag.h's own comment on why even the confirmed-correct framing here
     * is "for THIS module", not proven for every ILI9341-family panel this
     * codebase might one day support.
     *
     * Both fields are meaningless when ok is false. */
    uint16_t value;
    uint16_t alt_value;
    uint32_t read_us;
    bool ok;
};

/* The stats this command reports about one value stream: how far it ranges,
 * how many distinct values it took, and whether it trends up or down.
 * Shared by the primary and alternate framings (scanline_accumulate() below
 * fills one of these for each) so there is exactly one place this
 * accumulation logic can be wrong, not two near-identical copies of it. */
struct ScanlineStats {
    uint16_t value_min = 0;
    uint16_t value_max = 0;
    bool have_range = false;
    int distinct_count = 0;
    int increases = 0;
    int decreases = 0;
};

/* One pass over `samples`, extracting whichever framing (samples[i].value
 * when use_alt is false, samples[i].alt_value when true) and folding it
 * into *out. O(n^2) in kScanlineSampleCount for the distinct-value count,
 * same as before this function was split out -- 64 samples, so the total
 * work is trivial either way. */
void scanline_accumulate(const ScanlineSample *samples, int count, bool use_alt,
                          ScanlineStats *out) {
    bool have_prev = false;
    uint16_t prev = 0;
    for (int i = 0; i < count; ++i) {
        if (!samples[i].ok) {
            continue;
        }
        const uint16_t v = use_alt ? samples[i].alt_value : samples[i].value;

        if (!out->have_range) {
            out->value_min = v;
            out->value_max = v;
            out->have_range = true;
        } else {
            if (v < out->value_min) out->value_min = v;
            if (v > out->value_max) out->value_max = v;
        }

        bool seen_before = false;
        for (int j = 0; j < i; ++j) {
            if (!samples[j].ok) {
                continue;
            }
            const uint16_t pv = use_alt ? samples[j].alt_value : samples[j].value;
            if (pv == v) {
                seen_before = true;
                break;
            }
        }
        if (!seen_before) {
            out->distinct_count++;
        }

        if (have_prev) {
            if (v > prev) {
                out->increases++;
            } else if (v < prev) {
                out->decreases++;
            }
        }
        prev = v;
        have_prev = true;
    }
}

/* Builds the "every Nth sample, -1 for a failed one" representative array
 * this command reports, for whichever framing use_alt selects, into `out`
 * (capacity `cap`). Factored out so it can run twice (primary framing,
 * alternate framing) without duplicating the snprintf/bounds-check
 * bookkeeping. */
void scanline_build_sample_array(const ScanlineSample *samples, int count, bool use_alt,
                                  char *out, size_t cap) {
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < count; i += kScanlineReportStride) {
        char item[16];
        const uint16_t v = use_alt ? samples[i].alt_value : samples[i].value;
        const int item_n = samples[i].ok
            ? std::snprintf(item, sizeof item, "%s%u", off == 0 ? "" : ",",
                             static_cast<unsigned>(v))
            : std::snprintf(item, sizeof item, "%s-1", off == 0 ? "" : ",");
        if (item_n > 0 && off + static_cast<size_t>(item_n) < cap) {
            std::memcpy(out + off, item, static_cast<size_t>(item_n));
            off += static_cast<size_t>(item_n);
            out[off] = '\0';
        }
    }
}

void handle_scanline(uint32_t read_hz) {
    if (!require_read_line("KFDBG SCANLINE")) {
        return;
    }

    ScanlineSample *samples = static_cast<ScanlineSample *>(
        heap_caps_malloc(sizeof(ScanlineSample) * kScanlineSampleCount, MALLOC_CAP_SPIRAM));
    if (samples == nullptr) {
        KF_LOGE(TAG, "SCANLINE: out of PSRAM for %d samples", kScanlineSampleCount);
        reply_err("KFDBG SCANLINE", "out of memory sampling the scanline register");
        return;
    }

    /* Tear the write panel down and rebuild it at read_hz -- see
     * kf_esp_display_diag_begin_probe()'s own comment (esp_display.cpp) for
     * the full mechanics, and this file's own header comment for why doing
     * that here, on the frame-loop thread, is safe: nothing else draws
     * concurrently. A failed rebuild is NOT treated as a reason to skip the
     * sampling loop below -- kf_esp_display_diag_read_scanline() already
     * returns false cleanly whenever g_io is null, so the loop just records
     * 64 failures, which is itself the finding, reported via probe_ok and
     * failed==64 below rather than by silently returning early. */
    const bool probe_ok = kf_esp_display_diag_begin_probe(read_hz);
    if (!probe_ok) {
        KF_LOGE(TAG, "SCANLINE: could not rebuild the panel at %lu Hz for the "
                     "probe -- every sample below will report failure",
                static_cast<unsigned long>(read_hz));
    }

    int ok_count = 0;
    uint64_t total_us = 0;
    uint32_t min_us = 0;
    uint32_t max_us = 0;
    uint8_t first_raw[kScanlineReplyBytes] = {0, 0, 0};
    bool have_first_raw = false;

    for (int i = 0; i < kScanlineSampleCount; ++i) {
        uint8_t raw[kScanlineReplyBytes];
        const uint64_t t0 = kf_time_mono_us();
        const bool got = kf_esp_display_diag_read_scanline(raw, kScanlineReplyBytes);
        const uint64_t t1 = kf_time_mono_us();
        const uint32_t us = static_cast<uint32_t>(t1 - t0);

        samples[i].ok = got;
        samples[i].read_us = us;
        total_us += us;
        if (i == 0 || us < min_us) {
            min_us = us;
        }
        if (i == 0 || us > max_us) {
            max_us = us;
        }

        if (got) {
            if (!have_first_raw) {
                first_raw[0] = raw[0];
                first_raw[1] = raw[1];
                first_raw[2] = raw[2];
                have_first_raw = true;
            }
            ok_count++;
            /* value: the confirmed-correct framing (no dummy byte,
             * raw[0]/raw[1]) -- see ScanlineSample's own comment for why
             * this is "value" and not "alt_value" now. alt_value: the OLD
             * datasheet framing (1 dummy byte, raw[1]/raw[2]), kept for
             * comparison and for whatever panel profile needs it next.
             * Both masked to 0x03FF (10 bits, per the datasheet's own
             * field width). */
            samples[i].value = static_cast<uint16_t>(
                ((static_cast<uint16_t>(raw[0]) << 8) | raw[1]) & 0x03FFu);
            samples[i].alt_value = static_cast<uint16_t>(
                ((static_cast<uint16_t>(raw[1]) << 8) | raw[2]) & 0x03FFu);
        } else {
            samples[i].value = 0;
            samples[i].alt_value = 0;
        }
    }

    /* Restore the write clock unconditionally, and BEFORE any further
     * allocation below can fail and return early -- every remaining return
     * path in this function has therefore already put the panel back,
     * regardless of how the rest of the JSON build goes. See kf_esp_
     * display_diag_end_probe()'s own comment for what it means if this
     * restore itself fails (g_panel left null, degrading to no display
     * updates rather than a crash or a hang). */
    kf_esp_display_diag_end_probe();

    ScanlineStats primary{};
    ScanlineStats alt{};
    scanline_accumulate(samples, kScanlineSampleCount, /*use_alt=*/false, &primary);
    scanline_accumulate(samples, kScanlineSampleCount, /*use_alt=*/true, &alt);

    const uint32_t avg_us = static_cast<uint32_t>(total_us / kScanlineSampleCount);

    /* The representative sample arrays: every Nth sample, -1 for any that
     * failed, one per framing. Built into their own PSRAM buffers first,
     * then dropped into the main JSON below as two %s -- keeps the JSON
     * build a single readable snprintf, the same style every other handler
     * in this file uses. */
    constexpr size_t kSampleArrayCap = 256;
    char *sample_array = static_cast<char *>(heap_caps_malloc(kSampleArrayCap, MALLOC_CAP_SPIRAM));
    char *alt_sample_array = static_cast<char *>(heap_caps_malloc(kSampleArrayCap, MALLOC_CAP_SPIRAM));
    if (sample_array == nullptr || alt_sample_array == nullptr) {
        if (sample_array != nullptr) heap_caps_free(sample_array);
        if (alt_sample_array != nullptr) heap_caps_free(alt_sample_array);
        heap_caps_free(samples);
        KF_LOGE(TAG, "SCANLINE: out of PSRAM for the sample arrays");
        reply_err("KFDBG SCANLINE", "out of memory formatting result");
        return;
    }
    scanline_build_sample_array(samples, kScanlineSampleCount, /*use_alt=*/false,
                                 sample_array, kSampleArrayCap);
    scanline_build_sample_array(samples, kScanlineSampleCount, /*use_alt=*/true,
                                 alt_sample_array, kSampleArrayCap);

    char first_raw_hex[8];
    std::snprintf(first_raw_hex, sizeof first_raw_hex, "%02x%02x%02x", first_raw[0],
                  first_raw[1], first_raw[2]);

    constexpr size_t kJsonCap = 1536; /* was 1024; grown for the alt_* fields below */
    char *json = static_cast<char *>(heap_caps_malloc(kJsonCap, MALLOC_CAP_SPIRAM));
    if (json == nullptr) {
        heap_caps_free(alt_sample_array);
        heap_caps_free(sample_array);
        heap_caps_free(samples);
        KF_LOGE(TAG, "SCANLINE: out of PSRAM for the JSON reply");
        reply_err("KFDBG SCANLINE", "out of memory formatting result");
        return;
    }

    /* read_hz: the clock actually asked of kf_esp_display_diag_begin_probe()
     * above, whether or not the rebuild at that clock succeeded -- probe_ok
     * says which. changed_between_reads / alt_changed_between_reads:
     * distinct_count > 1 among the OK samples -- exactly one distinct value
     * (or zero, if every read failed) means the register never budged
     * across the whole run, which is what "reads are not working" looks
     * like from here. value_min/value_max/... are only meaningful when
     * ok > 0; -1 signals that plainly rather than reporting a misleading 0.
     * dummy_bytes_assumed is 0, not 1 -- it describes the unprefixed fields
     * below, which are now the confirmed no-dummy-byte framing; see
     * ScanlineSample's own comment for the swap and why. */
    const int n = std::snprintf(
        json, kJsonCap,
        "{\"cmd_hex\":\"0x%02x\",\"read_hz\":%lu,\"probe_ok\":%s,"
        "\"reply_bytes\":%u,\"dummy_bytes_assumed\":0,\"samples\":%d,"
        "\"ok\":%d,\"failed\":%d,\"first_raw_hex\":\"%s\",\"sample_stride\":%d,"
        "\"sample_values\":[%s],\"value_min\":%d,\"value_max\":%d,"
        "\"distinct_values\":%d,\"changed_between_reads\":%s,"
        "\"increases\":%d,\"decreases\":%d,"
        "\"alt_sample_values\":[%s],\"alt_value_min\":%d,\"alt_value_max\":%d,"
        "\"alt_distinct_values\":%d,\"alt_changed_between_reads\":%s,"
        "\"alt_increases\":%d,\"alt_decreases\":%d,"
        "\"avg_read_us\":%lu,\"min_read_us\":%lu,\"max_read_us\":%lu}",
        static_cast<unsigned>(KF_ESP_DISPLAY_DIAG_CMD_GET_SCANLINE),
        static_cast<unsigned long>(read_hz), probe_ok ? "true" : "false",
        static_cast<unsigned>(kScanlineReplyBytes), kScanlineSampleCount, ok_count,
        kScanlineSampleCount - ok_count, first_raw_hex, kScanlineReportStride,
        sample_array,
        primary.have_range ? static_cast<int>(primary.value_min) : -1,
        primary.have_range ? static_cast<int>(primary.value_max) : -1,
        primary.distinct_count, (primary.distinct_count > 1) ? "true" : "false",
        primary.increases, primary.decreases,
        alt_sample_array,
        alt.have_range ? static_cast<int>(alt.value_min) : -1,
        alt.have_range ? static_cast<int>(alt.value_max) : -1,
        alt.distinct_count, (alt.distinct_count > 1) ? "true" : "false",
        alt.increases, alt.decreases,
        static_cast<unsigned long>(avg_us), static_cast<unsigned long>(min_us),
        static_cast<unsigned long>(max_us));

    heap_caps_free(alt_sample_array);
    heap_caps_free(sample_array);
    heap_caps_free(samples);

    if (n <= 0 || static_cast<size_t>(n) >= kJsonCap) {
        KF_LOGE(TAG, "SCANLINE: JSON build failed or truncated (n=%d, cap=%zu)", n, kJsonCap);
        heap_caps_free(json);
        reply_err("KFDBG SCANLINE", "internal error formatting result");
        return;
    }

    kf_dbg_enqueue_reply("json", reinterpret_cast<uint8_t *>(json), static_cast<size_t>(n));
    heap_caps_free(json);
}

/* KFDBG VSYNC <0|1>: toggles esp_display.cpp's beam-racing wait at runtime
 * -- see kf_esp_display_vsync.h for the full contract and esp_display.cpp's
 * push_rect() for what the wait actually does. Takes effect immediately
 * (the very next push_rect() call reads the new setting; there is nothing
 * cached per frame to invalidate), which is the point: measure a run with
 * it on, flip it, measure again, all on the same board, no reflash.
 *
 * Refuses via require_read_line() on a panel with no read line -- turning
 * this on there would not be dangerous (push_rect()'s own
 * kf_vsync_read_scan_row() already fails closed and writes immediately when
 * a read fails, since MISO was never reserved on that profile either), but
 * it would be pointless and silently so: every call would pay for a doomed
 * SPI read attempt and get exactly the same behaviour as leaving this off.
 * Refusing tells a human that up front instead of letting them believe the
 * toggle did something. */
void handle_vsync(bool enable) {
    if (!require_read_line("KFDBG VSYNC")) {
        return;
    }

    kf_esp_display_vsync_set_enabled(enable);
    char content[32];
    const int n = std::snprintf(content, sizeof content, "VSYNC enabled=%d",
                                 enable ? 1 : 0);
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
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

/* KFDBG FEED/PLAY/REST/BATH <variation>: fires a care action against the
 * live pet, called DIRECTLY through kf_pet_session_feed()/_play()/_rest()/
 * _bath() -- not via KFDBG BTN's button-injection path, even though a real
 * button press reaches the exact same four functions through
 * kf_home_screen_input.cpp's kf_home_screen_handle_care_buttons(). Two
 * reasons this is its own command rather than reusing BTN:
 *
 *   1. kf_home_screen_handle_care_buttons()'s `variation` is an implicit,
 *      per-action cycling counter (0 -> 1 -> 2 -> 0 -> ...) private to
 *      that file,
 *      there purely as a keyboard-binding convenience (see that function's
 *      own header comment) -- it is not part of the session API. Driving
 *      care through BTN injection would force a scripted client to
 *      reconstruct that hidden counter (how many times has THIS action
 *      fired since the pet was last reset or jumped?) just to land on a
 *      specific variation. `kf_pet_feed()` et al.'s own `variation`
 *      argument is what actually decides whether the creature likes the
 *      care (kf/pet.h) -- over a scriptable serial link, taking it as an
 *      explicit argument here is strictly more useful than hiding it
 *      behind press-count state the caller cannot see or reset
 *      independently of the pet itself.
 *   2. A one-shot `KFDBG BTN` mask does not reliably register at all:
 *      Core's debounce (kDebounceUs, hakoniwaos/src/app.cpp) requires the
 *      SAME mask across consecutive ~33ms-apart polls before it produces a
 *      press edge, so BTN's single-poll injection is silently dropped
 *      unless the host holds it for >=120ms via BTNHOLD (see
 *      tools/kf_debug.py's `press` command and its own --hold-ms comment
 *      for the real bug this was found from). Calling kf_pet_session_*()
 *      directly sidesteps debounce entirely -- correct here because these
 *      commands exist to exercise the care action's own effect, not
 *      whether a simulated button press clears Core's debounce filter.
 *
 * `variation` is validated by process_command_line() (against
 * KF_PET_CARE_VARIATION_COUNT, kf/pet.h) before any of these run, so each
 * is only ever called with an already-in-range value. The ack reports
 * pet->last_reaction (kf_pet_reaction: 0=liked, 1=neutral, 2=disliked) as
 * a raw number, same convention KFDBG STATE's `stage`/`base_trait` fields
 * already use, rather than adding a second copy of
 * kf_home_screen_input.cpp's private reaction_name() text mapping. */
void handle_feed(uint8_t variation) {
    kf_pet_session_feed(variation);
    const kf_pet_state *pet = kf_pet_session_state();
    char content[64];
    const int n = std::snprintf(content, sizeof content,
                                 "FEED variation=%u reaction=%u",
                                 static_cast<unsigned>(variation),
                                 static_cast<unsigned>(pet->last_reaction));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_play(uint8_t variation) {
    kf_pet_session_play(variation);
    const kf_pet_state *pet = kf_pet_session_state();
    char content[64];
    const int n = std::snprintf(content, sizeof content,
                                 "PLAY variation=%u reaction=%u",
                                 static_cast<unsigned>(variation),
                                 static_cast<unsigned>(pet->last_reaction));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_rest(uint8_t variation) {
    kf_pet_session_rest(variation);
    const kf_pet_state *pet = kf_pet_session_state();
    char content[64];
    const int n = std::snprintf(content, sizeof content,
                                 "REST variation=%u reaction=%u",
                                 static_cast<unsigned>(variation),
                                 static_cast<unsigned>(pet->last_reaction));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_bath(uint8_t variation) {
    kf_pet_session_bath(variation);
    const kf_pet_state *pet = kf_pet_session_state();
    char content[64];
    const int n = std::snprintf(content, sizeof content,
                                 "BATH variation=%u reaction=%u",
                                 static_cast<unsigned>(variation),
                                 static_cast<unsigned>(pet->last_reaction));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* KFDBG FLUSH: kf_pet_session_flush() -- clears waiting poops. No
 * variation argument (kf_pet_flush() takes none) and no reaction reported:
 * kf_pet_flush() leaves last_reaction/last_care_action exactly as the
 * previous real care action left them (see hakoniwaos/src/pet.cpp's own
 * comment), so echoing either here would misattribute someone else's
 * reaction to a chore that has none of its own -- the same reasoning
 * kf_home_screen_input.cpp's kf_home_screen_handle_care_buttons() already
 * applies to its own FLUSH log line. */
void handle_flush() {
    kf_pet_session_flush();
    static const char kMsg[] = "FLUSH";
    kf_dbg_enqueue_reply("ack", reinterpret_cast<const uint8_t *>(kMsg),
                          sizeof(kMsg) - 1);
}

/* KFDBG JUMP <stage> [teen_form] [adult_branch]: kf_pet_session_debug_
 * jump_to_stage() -- puts the live pet at the START of `stage`, alive and
 * fully fed, without living through the real stage durations first (an
 * egg alone is a real hour; the full climb is close to a week -- see
 * kf_pet_session.h's own comment). Built for exactly the case CLAUDE.md's
 * task brief spells out: an uncared-for pet dies of neglect partway
 * through the child stage, and a dead pet is frozen permanently, so
 * without this a real device can never show later-stage sprites at all.
 *
 * `stage` is the raw kf_pet_stage enum value (0=egg .. 4=adult), same
 * convention KFDBG STATE's `stage` field already uses. `teen_form` and
 * `adult_branch` are optional and both default to 0 when omitted --
 * exactly kf_pet_session_debug_jump_to_stage()'s own "unset" behaviour
 * (see that function's header comment: 0 is always a valid family index
 * and every family has at least one adult, so 0 is a safe default for
 * both). `teen_form`'s valid range is [0, KF_PET_TEEN_FORM_DUST]
 * (kf/pet.h) INCLUSIVE -- KF_PET_TEEN_FORM_DUST is a real, reachable form
 * (deliberately equal to KF_PET_TEEN_FORM_COUNT, one past the four named
 * ones), not an error value, so this handler passes it through exactly
 * like any other in-range input rather than clamping it away; only input
 * genuinely out of range falls back to 0, inside
 * kf_pet_session_debug_jump_to_stage() itself. This handler does no range
 * validation of its own beyond "is it a decimal number" -- the session
 * function's own out-of-range-falls-back-to-0 contract is already the
 * complete, correct behaviour, and duplicating it here would just be a
 * second place for the two to drift apart.
 *
 * The ack reports the state that actually resulted (pet->stage/teen_form/
 * adult_branch, read back AFTER the call), not an echo of the request --
 * more useful for a scriptable client than the request alone, since
 * out-of-range teen_form/adult_branch silently fall back to 0 rather than
 * erroring, and this way a caller sees exactly what landed without a
 * separate KFDBG STATE round trip. */
void handle_jump(uint32_t stage, uint32_t teen_form, uint32_t adult_branch) {
    kf_pet_session_debug_jump_to_stage(static_cast<kf_pet_stage>(stage),
                                        static_cast<uint8_t>(teen_form),
                                        static_cast<uint8_t>(adult_branch));
    const kf_pet_state *pet = kf_pet_session_state();
    char content[80];
    const int n = std::snprintf(
        content, sizeof content, "JUMP stage=%d teen_form=%u adult_branch=%u",
        static_cast<int>(pet->stage), static_cast<unsigned>(pet->teen_form),
        static_cast<unsigned>(pet->adult_branch));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* KFDBG CLOCK DROWSY|BEDTIME|MORNING, or KFDBG CLOCK EPOCH <seconds>: moves
 * the world's clock, mirroring the desktop debug window's Drowsy/Bedtime/
 * Morning buttons (sdl_debug_window.cpp) plus one thing those buttons don't
 * offer -- an arbitrary epoch, for fixing a drifted DATE on real hardware
 * (see this file's header comment's "Time control" section, and ports/
 * esp32/README.md's former open question this closes).
 *
 * Both handlers below call kf_pet_session_debug_set_clock() DIRECTLY, never
 * kf_time_set_wall() -- see that session function's own header comment in
 * kf_pet_session.h for why: it moves the HAL wall clock AND Core's
 * last_advanced TOGETHER, which a bare kf_time_set_wall() call would not,
 * leaving Core still evaluating the night window against whatever time it
 * last saw. This file does not reimplement any of that reasoning, only
 * calls into it -- the same "call the session function directly" shape
 * ADVANCE/RESET/JUMP already use above.
 *
 * The named-point handler additionally calls kf_pet_session_debug_clock_
 * target(), never computing 21:50/22:00/07:00 itself -- those three times
 * are defined exactly once, in kf_pet_session.h, specifically so the
 * desktop buttons and this KFDBG verb can never drift apart the way an
 * earlier version of the desktop buttons once did against their own test
 * (see kf_pet_debug_clock_point's own comment for that history). */
void handle_clock_point(kf_pet_debug_clock_point point, const char *name) {
    const int64_t epoch = kf_pet_session_debug_clock_target(point);
    kf_pet_session_debug_set_clock(epoch);
    char content[64];
    const int n = std::snprintf(content, sizeof content,
                                 "CLOCK point=%s epoch=%lld", name,
                                 static_cast<long long>(epoch));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_clock_epoch(int64_t epoch_seconds) {
    kf_pet_session_debug_set_clock(epoch_seconds);
    char content[48];
    const int n = std::snprintf(content, sizeof content, "CLOCK epoch=%lld",
                                 static_cast<long long>(epoch_seconds));
    kf_dbg_enqueue_reply("ack", reinterpret_cast<uint8_t *>(content),
                          n > 0 ? static_cast<size_t>(n) : 0);
}

/* KFDBG RTC: reads the DS3231 directly over I2C via kf_esp_time_debug.h's
 * one accessor -- NOT kf_time_wall(), which is the in-RAM clock and never
 * touches the bus after boot. See this file's header comment's "RTC"
 * section for why that distinction is the whole point, and kf_esp_time_
 * debug.h for exactly what kf_esp_time_debug_read_rtc() reports and when
 * it returns false.
 *
 * Observe tier: no require_mutate_enabled() call, unlike every handler
 * above this one in the file -- reading a chip's registers changes
 * nothing, the same reasoning PING/SHOT/STATE/SCANLINE/VSYNC already rest
 * on (see this file's header comment).
 *
 * `present` is always true in the `json` reply below -- when no chip ever
 * answered at boot, this replies `err` instead (see the read_ok check),
 * the same "bail before building a reply with nothing to report" shape
 * require_read_line() already uses for SCANLINE/VSYNC on a panel with no
 * read line. Kept as an explicit field rather than removed once redundant
 * with "did KFDBG RTC reply err" specifically so a host script parsing the
 * `json` reply never needs to branch on frame type to know it -- the same
 * "the field says what the type already implies" convenience over_budget
 * and vsync_enabled already provide elsewhere in this file.
 *
 * `wall`/`wall_valid`: kf_time_wall()'s own two fields, alongside `epoch`/
 * `osf` in the SAME reply so a host can compare RAM clock against physical
 * chip in one line, rather than needing a second KFDBG STATE round trip
 * (which does not carry wall/wall_valid at all) racing a clock that could
 * tick between the two requests. */
void handle_rtc() {
    int64_t epoch = 0;
    bool osf = false;
    const bool read_ok = kf_esp_time_debug_read_rtc(&epoch, &osf);
    if (!read_ok) {
        reply_err("KFDBG RTC", "no DS3231 answered at boot -- nothing to "
                                "read (see esp_time.cpp's try_init_ds3231())");
        return;
    }

    const kf_wall_time wall = kf_time_wall();

    char json[192];
    const int n = std::snprintf(
        json, sizeof json,
        "{\"present\":true,\"epoch\":%lld,\"osf\":%s,\"wall\":%lld,"
        "\"wall_valid\":%s}",
        static_cast<long long>(epoch), osf ? "true" : "false",
        static_cast<long long>(wall.epoch_seconds),
        wall.valid ? "true" : "false");

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(json)) {
        KF_LOGE(TAG, "RTC: JSON build failed or truncated (n=%d)", n);
        reply_err("KFDBG RTC", "internal error formatting result");
        return;
    }
    kf_dbg_enqueue_reply("json", reinterpret_cast<uint8_t *>(json),
                          static_cast<size_t>(n));
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

/* Same shape as parse_decimal() above, 64 bits wide -- KFDBG CLOCK EPOCH's
 * argument is an epoch second handed straight to kf_pet_session_debug_
 * set_clock(int64_t), and uint32_t (parse_decimal()'s width) only reaches
 * to the year 2106. Not a signed parser, same as parse_decimal() above: the
 * C standard lets strtoull() accept a leading '-' and wrap the result
 * rather than rejecting it, a quirk this file already lives with for every
 * OTHER decimal argument (ADVANCE seconds, MULT, JUMP's stage/teen_form/
 * adult_branch, ...) rather than fixing it here alone. In practice this
 * only matters for a hand-typed negative epoch, which is not a real
 * request this project has: every legitimate epoch is positive. */
bool parse_decimal64(const char *tok, uint64_t *out) {
    char *endp = nullptr;
    const unsigned long long v = std::strtoull(tok, &endp, 10);
    if (endp == tok || *endp != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(v);
    return true;
}

/* KFDBG CLOCK's first argument: DROWSY/BEDTIME/MORNING map to
 * kf_pet_session.h's three named points; anything else (including "EPOCH",
 * handled separately by process_command_line() below) is not one of these
 * three, so this returns false rather than erroring itself -- the caller
 * decides what a non-match means. Uppercase only, matching every other
 * KFDBG subcommand and argument keyword in this file (PING, BTN's button
 * names are the one exception, and those come through kf_debug.py's own
 * BUTTON_BITS table, not this parser). */
bool parse_clock_point(const char *tok, kf_pet_debug_clock_point *out) {
    if (std::strcmp(tok, "DROWSY") == 0) {
        *out = KF_PET_DEBUG_CLOCK_DROWSY;
        return true;
    }
    if (std::strcmp(tok, "BEDTIME") == 0) {
        *out = KF_PET_DEBUG_CLOCK_BEDTIME;
        return true;
    }
    if (std::strcmp(tok, "MORNING") == 0) {
        *out = KF_PET_DEBUG_CLOCK_MORNING;
        return true;
    }
    return false;
}

/* Shared by KFDBG FEED/PLAY/REST/BATH's four otherwise-identical parse
 * steps: one required decimal token, range-checked against
 * KF_PET_CARE_VARIATION_COUNT (kf/pet.h) -- Core's own count of how many
 * ways there are to do each action, not a number this file hardcodes.
 * Replies with `err` and returns false on any failure, exactly like every
 * other inline validation in process_command_line() below. */
bool parse_care_variation(const char *&p, const char *line, uint8_t *out) {
    char tok[16];
    uint32_t v = 0;
    if (!next_token(p, tok, sizeof tok) || !parse_decimal(tok, &v)) {
        reply_err(line, "needs one decimal variation argument "
                         "(0..KF_PET_CARE_VARIATION_COUNT-1)");
        return false;
    }
    if (v >= KF_PET_CARE_VARIATION_COUNT) {
        reply_err(line, "variation out of range "
                         "(0..KF_PET_CARE_VARIATION_COUNT-1)");
        return false;
    }
    *out = static_cast<uint8_t>(v);
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
    } else if (std::strcmp(tok1, "RTC") == 0) {
        /* Observe tier, like PING/SHOT/STATE/SCANLINE/VSYNC above -- no
         * require_mutate_enabled() call. See handle_rtc()'s own comment. */
        handle_rtc();
    } else if (std::strcmp(tok1, "SCANLINE") == 0) {
        /* Optional trailing arg: a decimal read clock in Hz, so a human can
         * try 1MHz or 4MHz from the host without a firmware rebuild -- see
         * kScanlineDefaultReadHz's own comment for why 2MHz is the default
         * when this is omitted. */
        char tok2[16];
        uint32_t read_hz = kScanlineDefaultReadHz;
        if (next_token(p, tok2, sizeof tok2)) {
            if (!parse_decimal(tok2, &read_hz) || read_hz == 0) {
                reply_err(line, "KFDBG SCANLINE's optional read_hz must be a "
                                 "positive decimal number of Hz");
                return;
            }
        }
        handle_scanline(read_hz);
    } else if (std::strcmp(tok1, "BTN") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        char tok2[16];
        uint32_t mask = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &mask)) {
            reply_err(line, "KFDBG BTN needs one decimal mask argument");
            return;
        }
        handle_btn(mask);
    } else if (std::strcmp(tok1, "BTNHOLD") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
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
        if (!require_mutate_enabled(line)) {
            return;
        }
        char tok2[16];
        uint32_t seconds = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &seconds)) {
            reply_err(line, "KFDBG ADVANCE needs one decimal seconds argument");
            return;
        }
        handle_advance(seconds);
    } else if (std::strcmp(tok1, "RESET") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        handle_reset();
    } else if (std::strcmp(tok1, "MULT") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
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
    } else if (std::strcmp(tok1, "CLOCK") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        char tok2[16];
        if (!next_token(p, tok2, sizeof tok2)) {
            reply_err(line, "KFDBG CLOCK needs DROWSY, BEDTIME, MORNING, or "
                             "EPOCH <seconds>");
            return;
        }
        kf_pet_debug_clock_point point{};
        if (parse_clock_point(tok2, &point)) {
            handle_clock_point(point, tok2);
        } else if (std::strcmp(tok2, "EPOCH") == 0) {
            char tok3[24];
            uint64_t epoch_u = 0;
            if (!next_token(p, tok3, sizeof tok3) ||
                !parse_decimal64(tok3, &epoch_u)) {
                reply_err(line, "KFDBG CLOCK EPOCH needs one decimal seconds "
                                 "argument");
                return;
            }
            handle_clock_epoch(static_cast<int64_t>(epoch_u));
        } else {
            reply_err(line, "KFDBG CLOCK's argument must be DROWSY, "
                             "BEDTIME, MORNING, or EPOCH <seconds>");
            return;
        }
    } else if (std::strcmp(tok1, "VSYNC") == 0) {
        char tok2[16];
        uint32_t v = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &v) ||
            (v != 0u && v != 1u)) {
            reply_err(line, "KFDBG VSYNC needs one argument, 0 or 1");
            return;
        }
        handle_vsync(v != 0u);
    } else if (std::strcmp(tok1, "FEED") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        uint8_t variation = 0;
        if (!parse_care_variation(p, line, &variation)) {
            return;
        }
        handle_feed(variation);
    } else if (std::strcmp(tok1, "PLAY") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        uint8_t variation = 0;
        if (!parse_care_variation(p, line, &variation)) {
            return;
        }
        handle_play(variation);
    } else if (std::strcmp(tok1, "REST") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        uint8_t variation = 0;
        if (!parse_care_variation(p, line, &variation)) {
            return;
        }
        handle_rest(variation);
    } else if (std::strcmp(tok1, "BATH") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        uint8_t variation = 0;
        if (!parse_care_variation(p, line, &variation)) {
            return;
        }
        handle_bath(variation);
    } else if (std::strcmp(tok1, "FLUSH") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        handle_flush();
    } else if (std::strcmp(tok1, "JUMP") == 0) {
        if (!require_mutate_enabled(line)) {
            return;
        }
        /* One required arg (stage), two optional (teen_form, adult_branch)
         * -- both default to 0 when omitted, matching kf_pet_session_
         * debug_jump_to_stage()'s own "unset" default. See handle_jump()'s
         * own comment for why no further range validation happens here. */
        char tok2[16];
        char tok3[16];
        char tok4[16];
        uint32_t stage = 0;
        uint32_t teen_form = 0;
        uint32_t adult_branch = 0;
        if (!next_token(p, tok2, sizeof tok2) || !parse_decimal(tok2, &stage)) {
            reply_err(line, "KFDBG JUMP needs at least a decimal stage "
                             "argument (0=egg..4=adult)");
            return;
        }
        if (next_token(p, tok3, sizeof tok3)) {
            if (!parse_decimal(tok3, &teen_form)) {
                reply_err(line, "KFDBG JUMP's teen_form argument must be decimal");
                return;
            }
            if (next_token(p, tok4, sizeof tok4)) {
                if (!parse_decimal(tok4, &adult_branch)) {
                    reply_err(line, "KFDBG JUMP's adult_branch argument must be decimal");
                    return;
                }
            }
        }
        handle_jump(stage, teen_form, adult_branch);
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

    KF_LOGI(TAG, "KFDBG bridge up on UART%d (console baud), mutate %s, input inject %s",
            static_cast<int>(kUartNum), KF_DBG_MUTATE_ENABLE ? "ON" : "OFF",
            KF_DBG_INPUT_INJECT_ENABLE ? "ON" : "OFF");
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
