/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Owns the ONE live kf_pet_state this build cares about right now, and
 * drives its two "ages" against the actual HAL, the same distinction
 * kf/hal/time.h draws for the two clocks:
 *
 *   kf_pet_session_init()   Boot-time: load the save (or start fresh) and
 *                           fast-forward it by however long the wall clock
 *                           says the device was off. The "aged while
 *                           switched off" half -- see
 *                           kf_pet_load_and_advance().
 *
 *   kf_pet_session_frame()  Every frame while running: advance the pet by
 *                           the LIVE elapsed time since the last call. The
 *                           "ages while you are playing" half. Deliberately
 *                           NOT driven by the wall clock -- see
 *                           kf/hal/time.h's own warning about conflating
 *                           the two.
 *
 * This is a simulator-only orchestration layer, not Core: kf/pet.h's
 * functions are pure and take an explicit kf_pet_state*, by design (see
 * ADR 0015), so they stay trivially unit-testable with no notion of "the"
 * pet. Something has to own the one instance a running build actually
 * uses, and give it a name other modules -- the Lua binding (ADR 0016),
 * later a UI -- can reach without each inventing their own copy. This is
 * that something. Lives in simulator/, not hakoniwaos/, for the same
 * reason kf_lvgl_port and kf_lua_port do: this does not claim the ESP32
 * build has a wired-up pet session, only that this slice's desktop/
 * headless backends do.
 */

#ifndef KF_PET_SESSION_H
#define KF_PET_SESSION_H

#include "kf/pet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings the active pet up: kf_pet_default_config(), then
 * kf_pet_load_and_advance() against KF_PET_SAVE_KEY -- a fresh pet if
 * there is no save yet, or an existing one fast-forwarded for however long
 * the wall clock says passed since it was last saved. Call once at boot;
 * kf_pet_session_shutdown() undoes it. Requires kf_store_init() and
 * kf_power_init() (kf/hal/storage.h, kf/hal/power.h) to already be up --
 * this does not bring up the HAL itself, the same division
 * run_storage_power_check() already established. kf_app_init() already
 * brings both up, so anything called after it is fine. */
void kf_pet_session_init(void);

/* Advances the active pet by the LIVE elapsed time since the last call (or
 * since kf_pet_session_init(), for the first call). Same 0-means-real-time
 * convention as kf_lvgl_port_pump()/kf_lua_port_frame(): 0 means "use real
 * elapsed time" (tracked internally via kf_time_mono_us()); headless
 * always passes a fixed synthetic period so a test run takes milliseconds,
 * not real minutes.
 *
 * Does NOT call kf_pet_advance() every frame, or even every whole second.
 * It batches: elapsed milliseconds accumulate here across many calls, and
 * only get flushed into one kf_pet_advance() call once at least
 * KF_PET_SESSION_FLUSH_SECONDS have built up. This is not an optimisation,
 * it is a correctness requirement -- kf_pet_advance()'s decay formula is
 * `rate_mp_per_hour * elapsed_seconds / 3600`, integer division, and for
 * every one of this project's configured rates (all under 3600 mp/hour) a
 * single call with elapsed_seconds under about 4-7 (rate-dependent)
 * truncates to EXACTLY ZERO. Flushing once per whole second, the more
 * "obvious" design, would mean a live-ticking pet never ages at all, no
 * matter how long the session runs: each 1-second call independently
 * starts from a full-precision `value` and throws away its own fractional
 * remainder every time, with no memory of what the previous call
 * discarded. Batching a larger elapsed value into fewer calls is what
 * lets that fraction actually accumulate to something the division can
 * see. This is a genuine, deliberate trade against kf_pet_advance()'s own
 * exactness: a live session slightly UNDER-decays relative to true
 * continuous-time decay (by at most one flush interval's worth per flush,
 * always in the generous direction, never negative, never compounding
 * unboundedly across flushes) -- see ADR 0016 for the numbers. Offline
 * fast-forward (kf_pet_load_and_advance(), called once at boot with
 * however many real seconds actually passed) is unaffected and remains
 * exact; this trade-off is specific to the per-frame live path. */
void kf_pet_session_frame(uint32_t synthetic_frame_delta_ms);

/* How long a burst of live elapsed time must accumulate before
 * kf_pet_session_frame() actually applies it. See that function's comment
 * for why this exists at all. 30 seconds keeps the worst-case per-flush
 * truncation loss small relative to an hour-scale decay rate (a few
 * percent, not the ~100% loss a 1-second flush would suffer) while still
 * being imperceptible against rates that take DAYS to empty a need. */
#define KF_PET_SESSION_FLUSH_SECONDS 30u

/* Read-only view of the live pet -- for the Lua binding (ADR 0016), and
 * later a UI screen. Never NULL after kf_pet_session_init(); asserts if
 * called before it, the same "host wiring order, not user input" contract
 * every other *_init()-gated accessor in this codebase uses. */
const kf_pet_state *kf_pet_session_state(void);

/* Care actions against the live pet. See kf/pet.h's kf_pet_feed() etc. --
 * these are exactly those functions, called against the one instance this
 * module owns. */
void kf_pet_session_feed(void);
void kf_pet_session_play(void);
void kf_pet_session_rest(void);

/* Persists the active pet's state now, via kf_pet_save(). Called
 * automatically by kf_pet_session_shutdown(), and exposed here too (and to
 * Lua, via the pet.save() binding) so a caller can save at a meaningful
 * checkpoint -- e.g. right after a care action -- rather than only at
 * exit, which on the device may not run at all: see kf/hal/power.h's
 * warning that kf_power_deep_sleep_until() may not return in the normal
 * sense. A failed save is logged, not fatal -- a full NVS partition
 * (KF_ERR_EXHAUSTED) should not crash the device, it should be visible in
 * the log and tried again next checkpoint. */
void kf_pet_session_save(void);

/* Saves, then tears the session down. */
void kf_pet_session_shutdown(void);

/* ---------------------------------------------------------------------
 * DEBUG ONLY below this line. Not part of the gameplay surface (not
 * exposed to Lua -- a script has no business fast-forwarding or resetting
 * time on its own pet), not called by the ESP32 build, and not something
 * a real device would ever expose to a player. Their only current caller
 * is sdl_main.cpp's debug key bindings, added specifically because
 * kf_pet_advance()'s bounded-loop design (ADR 0021) makes jumping an
 * arbitrary amount of pet-time cheap and safe to call directly, and the
 * default illustrative stage durations (an hour for an egg, about a week
 * for a full grow-up) are otherwise much too slow to watch interactively.
 * --------------------------------------------------------------------- */

/* Advances the live pet by exactly `seconds`, immediately -- bypassing
 * kf_pet_session_frame()'s live-tick batching (KF_PET_SESSION_FLUSH_
 * SECONDS) entirely, since that batching exists to make small per-frame
 * deltas add up correctly over real time, not to gate a deliberate,
 * one-shot jump. Uses the exact same kf_pet_advance() offline fast-forward
 * relies on, so this is not a separate, less-tested code path -- it is
 * the same one, called on demand instead of at boot. */
void kf_pet_session_debug_advance(uint32_t seconds);

/* Resets the live pet to a fresh egg, in place -- without touching
 * whatever is currently on disk. The next normal checkpoint (a care
 * action, or shutdown) overwrites the save with this fresh state. Lets
 * someone testing branch outcomes start over without restarting the
 * whole process or deleting a save file by hand. */
void kf_pet_session_debug_reset(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_PET_SESSION_H */
