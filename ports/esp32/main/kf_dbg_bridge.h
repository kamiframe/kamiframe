/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The KFDBG serial debug bridge: lets a developer -- or an AI assistant
 * with no camera and no hands -- see and drive the real device over the
 * same UART `idf.py monitor` already uses, instead of photographing the
 * screen. Full protocol and rationale: docs/architecture/
 * adr-0030-serial-debug-bridge.md. The host side is tools/kf_debug.py,
 * already written and passing its own selftest (tools/kf_debug_selftest.py)
 * against this exact wire format -- this file is the other half of an
 * already-fixed contract, not a fresh design.
 *
 * ============================================================================
 *  WHY A BACKGROUND TASK, NOT A NON-BLOCKING POLL IN THE MAIN LOOP
 *
 *  The protocol spec requires the frame loop never stall for this. Two
 *  shapes were open: poll a few non-blocking bytes off the UART once per
 *  frame from inside app_main.cpp's own loop, or hand the UART to a
 *  dedicated low-priority FreeRTOS task and talk to it over queues. This
 *  file is the second.
 *
 *  Reading is the easy half either way -- a non-blocking `uart_read_bytes`
 *  with a zero timeout would have worked fine from the main loop too.
 *  WRITING is what forces the choice. `KFDBG SHOT`'s reply is a
 *  framebuffer, RLE-compressed then base64'd; even a well-compressed real
 *  screen (see the ADR's worked example: ~2KB base64 for a HUD-shaped test
 *  image) takes tens of milliseconds to clock out at this bridge's fixed
 *  115200 baud, and an incompressible one (the protocol's own explicit
 *  worst case) would take tens of *seconds*. `uart_write_bytes()` blocks
 *  once the driver's TX path can't keep up. Call that inline from
 *  app_main.cpp's loop body and every frame after the SHOT command is late
 *  by however long the transmit took -- exactly the stall the spec
 *  forbids, and for far longer than the read side would ever cause on its
 *  own.
 *
 *  So the split here is not "one task does I/O" but specifically "the slow
 *  half of I/O never runs on the frame-loop thread, at all":
 *
 *    kf_dbg_bridge_frame()  Called once per iteration, from app_main.cpp's
 *                            loop, BEFORE kf_app_frame() (see that file's
 *                            comment on why the ordering matters for BTN).
 *                            Does a bounded, non-blocking amount of work:
 *                            pop at most one already-assembled command line
 *                            off a queue, and if there is one, build its
 *                            reply -- which touches kf_fb_pixels(),
 *                            kf_pet_session_state() etc., so it has to run
 *                            on this thread, not a background one, to avoid
 *                            racing the frame that's about to render -- then
 *                            hand the finished reply, ALREADY FULLY BUILT
 *                            AS BYTES, to a second queue. No UART write
 *                            happens on this thread. Ever.
 *
 *    kf_dbg_rx_task()        A dedicated low-priority task
 *    kf_dbg_tx_task()        (KF_DBG_TASK_PRIORITY, tskIDLE_PRIORITY + 1 --
 *                            see that constant's own comment) that owns the
 *                            UART for reading and writing respectively.
 *                            Both block on I/O almost all the time (a
 *                            blocking `uart_read_bytes`/queue receive),
 *                            which is exactly what a low-priority task
 *                            should do: contend for the CPU only when
 *                            there is genuinely nothing else for them to
 *                            wait on.
 *
 *  A consequence worth being explicit about: because command *parsing and
 *  execution* happens on the main frame-loop thread and the injected-input
 *  state (KFDBG BTN/BTNHOLD) is read by kf_input_poll(), also on the main
 *  thread, that shared state needs no lock at all -- see kf_dbg_input_
 *  mask()'s own comment. That fell out of this split; it was not the
 *  reason for it.
 * ============================================================================
 */

#ifndef KF_DBG_BRIDGE_H
#define KF_DBG_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Compile-time flags. THREE, split by what they gate, not by how big the
 * gate is: a master switch for the whole bridge, and two narrower ones
 * nested inside it -- one for every command that MUTATES the pet or the
 * simulation, one (nested a level further, inside that) for button
 * injection specifically. See ADR 0035 for why the boundary moved here:
 * the original two-flag split (this file's history, and ADR 0030) drew the
 * line at "button injection vs. everything else", which left ADVANCE/
 * RESET/MULT/FEED/PLAY/REST/BATH/FLUSH/JUMP -- every command that actually
 * changes the pet's state or the simulation's clock -- reachable with only
 * the master switch on, regardless of the narrower flag's setting. For a
 * pet whose whole premise is that time and care are real, that let a
 * serial cable fake both. The line now runs observe vs. mutate instead:
 *
 *   KF_DBG_BRIDGE_ENABLE   the whole channel. Off means none of this
 *                          exists in the binary (see its own comment
 *                          below) -- neither flag beneath it means
 *                          anything without it.
 *   KF_DBG_MUTATE_ENABLE   every command that changes the pet or the
 *                          simulation: FEED/PLAY/REST/BATH/FLUSH/JUMP/
 *                          ADVANCE/RESET/MULT/CLOCK/BTN/BTNHOLD. Off leaves
 *                          PING/SHOT/STATE/SCANLINE/VSYNC/RTC (pure
 *                          introspection) working -- a serial connection
 *                          can watch the device, never drive it.
 *   KF_DBG_INPUT_INJECT_ENABLE   nested inside KF_DBG_MUTATE_ENABLE:
 *                          narrows the mutate set further to exclude
 *                          JUST BTN/BTNHOLD. See its own comment below
 *                          for why this narrower cut still earns its
 *                          keep now that mutation as a whole has its own
 *                          flag.
 *
 * All three default ON. This is pre-release developer firmware with no
 * user on the other end of that UART yet; see ADR 0035's "Shipping a
 * build with this off" section for exactly what to flip, and where,
 * before this ever reaches a product. -------------------------------- */

/* The whole bridge: UART ownership, both tasks, both queues, every KFDBG
 * command. 0 compiles every one of kf_dbg_bridge_init()/_frame()/
 * _shutdown() down to an empty function body -- no task, no queue, no
 * uart_driver_install() call, nothing -- so "disabled" means genuinely
 * absent from the binary, not merely unreachable at runtime. Override with
 * -DKF_DBG_BRIDGE_ENABLE=0 (see ports/esp32/main/CMakeLists.txt for where
 * a shipping build would set this). */
#ifndef KF_DBG_BRIDGE_ENABLE
#define KF_DBG_BRIDGE_ENABLE 1
#endif

/* Every command that mutates the pet or the simulation: FEED, PLAY, REST,
 * BATH, FLUSH, JUMP, ADVANCE, RESET, MULT, CLOCK, BTN, BTNHOLD -- see
 * ADR 0035 (CLOCK joined the set later, under ADR 0054, on the identical
 * gate, for the identical reason: it changes the pet's own notion of the
 * time of day). 0 makes process_command_line() (kf_dbg_bridge.cpp) reply
 * `err` to each of those, naming this flag, instead of running them;
 * PING/SHOT/STATE/SCANLINE/VSYNC/RTC are untouched by this flag, since none
 * of them changes anything. Independent of KF_DBG_INPUT_INJECT_ENABLE
 * below -- that flag
 * narrows the mutate set further, it does not widen it -- so setting only
 * this one to 0 is what a build wants for "observation only": the bridge
 * stays up, screenshots and state reads keep working, nothing can touch
 * the pet. Meaningless whenever the bridge as a whole is off, same
 * "no channel to arrive on" reasoning KF_DBG_INPUT_INJECT_ENABLE's own
 * comment already gives for BTN/BTNHOLD specifically. */
#ifndef KF_DBG_MUTATE_ENABLE
#define KF_DBG_MUTATE_ENABLE KF_DBG_BRIDGE_ENABLE
#endif

/* Button injection specifically (KFDBG BTN / KFDBG BTNHOLD OR-ing into
 * esp_input.cpp's real GPIO reads) -- nested inside KF_DBG_MUTATE_ENABLE
 * above, not the bridge flag directly, since BTN/BTNHOLD are themselves
 * two of the twelve commands that flag already gates (ADR 0035, ADR 0054).
 * Kept as its own flag, rather than folded away now that mutation as a
 * whole has a gate, because button injection is a strictly larger attack
 * surface than every other mutating command put together: FEED/PLAY/REST/
 * BATH/FLUSH/JUMP/ADVANCE/RESET/MULT/CLOCK each has one bounded,
 * enumerable effect on the pet, but a BTN mask can drive ANY UI flow
 * reachable by button presses -- including menu screens this file has
 * never heard of and never will need to. A build that wants remote care
 * actions and time control (say, for a support technician) without
 * handing a serial
 * connection the ability to navigate arbitrary menus can set
 * KF_DBG_INPUT_INJECT_ENABLE=0 while leaving KF_DBG_MUTATE_ENABLE=1; a
 * build with mutation off entirely gets this for free, since nothing
 * beneath an off master switch matters. Meaningless (and, per
 * kf_dbg_input_mask()'s own #if, compiled to an unconditional `return 0`)
 * whenever the bridge as a whole is off, since BTN/BTNHOLD have no command
 * channel to arrive on in that case. */
#ifndef KF_DBG_INPUT_INJECT_ENABLE
#define KF_DBG_INPUT_INJECT_ENABLE KF_DBG_MUTATE_ENABLE
#endif

/* --------------------------------------------------------------------------
 * Lifecycle. Called unconditionally from app_main.cpp regardless of the
 * flags above -- callers never need an #if of their own; see this file's
 * "WHY A BACKGROUND TASK" comment and the ADR for what each call does when
 * KF_DBG_BRIDGE_ENABLE is 0 (nothing at all). -------------------------- */

/* Installs the UART driver's RX path on the console UART (whatever
 * ESP_CONSOLE_UART_NUM is -- CONFIG says 0 on this build) and starts both
 * tasks. Call once, after kf_app_init() (this does not depend on it
 * directly, but every command handler it will eventually run does: SHOT
 * needs kf_fb_init() already called, STATE needs the pet session already
 * up). Does not configure baud rate or pins -- the console's own startup
 * already did, and re-configuring here would risk fighting it; see the
 * ADR's "Not verified" section for the one real risk this leaves open. */
void kf_dbg_bridge_init(void);

/* Call once per iteration of app_main.cpp's frame loop, BEFORE
 * kf_app_frame() -- see this file's header comment for why the ordering
 * matters. Non-blocking: pops at most one already-received command line,
 * and if there is one, builds and enqueues its reply. Every command
 * handler's own work (base64/RLE encoding, JSON formatting) runs here, on
 * the calling thread -- bounded CPU time, never I/O. */
void kf_dbg_bridge_frame(void);

/* Stops both tasks and releases the UART driver. Mirrors every other
 * *_shutdown() in this codebase (see app_main.cpp) even though, like
 * kf_input_shutdown(), it is unreachable on this backend today -- correct
 * shape if that ever changes, dead code if it doesn't. */
void kf_dbg_bridge_shutdown(void);

/* --------------------------------------------------------------------------
 * The input-injection side of the contract with esp_input.cpp. Declared
 * unconditionally, like every *_init()-adjacent accessor in this codebase
 * (see kf_pet_session.h's own comment on the same pattern), so
 * esp_input.cpp never needs an #if of its own around the call site. ---- */

/* The button mask KFDBG BTN/BTNHOLD most recently asked for, still live
 * this frame -- 0 if nothing is currently injected. esp_input.cpp ORs
 * this into the real GPIO read in kf_input_poll(), never replaces it, so
 * physical buttons keep working while an injection is active (see
 * esp_input.cpp's own comment on why OR and not assignment).
 *
 * Called exactly once per frame, from kf_input_poll(), on the SAME thread
 * that kf_dbg_bridge_frame() runs on (the main frame-loop thread) and that
 * sets the state this reads (BTN/BTNHOLD's handlers, inside kf_dbg_bridge_
 * frame() -> kf_app_frame() -> kf_input_poll(), all one thread, one frame,
 * in that order -- see app_main.cpp's loop). No other thread ever touches
 * this state. That is what makes plain (non-atomic, unlocked) globals the
 * correct implementation here, not a shortcut taken because a lock was
 * inconvenient -- see kf_dbg_bridge.cpp's own comment on the state this
 * function reads for the mechanics (one-shot BTN vs timed BTNHOLD). */
uint32_t kf_dbg_input_mask(void);

/* --------------------------------------------------------------------------
 * The time-control side of the contract with app_main.cpp. Declared
 * unconditionally, same reasoning as kf_dbg_input_mask() above. See
 * kf_dbg_bridge.cpp's handle_advance()/handle_reset()/handle_mult() for
 * the KFDBG ADVANCE/RESET/MULT commands that drive this. ---------------- */

/* The time multiplier KFDBG MULT most recently set, still in effect this
 * frame -- 1 (real time, unscaled) until a MULT command arrives, same
 * "1 until told otherwise" default sdl_debug_window.cpp's play-speed
 * multiplier uses. app_main.cpp's loop multiplies this into the delta it
 * hands kf_pet_session_frame() -- and ONLY that delta, not LVGL's tick or
 * Lua's frame delta -- for the exact reason sdl_main.cpp's own identical
 * call already documents: see that file's comment on
 * kf_sdl_debug_window_time_multiplier(). Range is 1..256, matching the
 * simulator's debug window; KFDBG MULT rejects anything outside that
 * range with an `err` reply rather than storing it here, so this always
 * returns a value already known to be in range.
 *
 * Called exactly once per frame, from app_main.cpp's loop, on the SAME
 * thread that kf_dbg_bridge_frame() runs on and that sets this value
 * (MULT's handler, inside kf_dbg_bridge_frame()) -- same single-thread,
 * no-lock-needed reasoning as kf_dbg_input_mask() above. */
uint32_t kf_dbg_time_multiplier(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_DBG_BRIDGE_H */
