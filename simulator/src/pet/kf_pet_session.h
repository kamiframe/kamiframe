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
 * exact; this trade-off is specific to the per-frame live path.
 *
 * Also where two of kf_pet_session_save()'s automatic checkpoints live
 * (see that function's own comment for the full list): every flush that
 * crosses the awake -> asleep transition saves immediately, and every
 * flush otherwise checks whether the periodic safety-net checkpoint is
 * due. Both are ordinary calls to kf_pet_session_save() -- no separate
 * code path -- so a failed write is logged and retried exactly the way a
 * manual save would be. */
void kf_pet_session_frame(uint32_t synthetic_frame_delta_ms);

/* How long a burst of live elapsed time must accumulate before
 * kf_pet_session_frame() actually applies it. See that function's comment
 * for why this exists at all. 30 seconds keeps the worst-case per-flush
 * truncation loss small relative to an hour-scale decay rate (a few
 * percent, not the ~100% loss a 1-second flush would suffer) while still
 * being imperceptible against rates that take DAYS to empty a need. */
#define KF_PET_SESSION_FLUSH_SECONDS 30u

/* How often kf_pet_session_frame() re-checks whether a periodic safety-net
 * save is due -- the "periodically while running" checkpoint alongside
 * "after every care action" and "at the awake -> asleep transition" (see
 * kf_pet_session_save()'s own comment for the full checkpoint list). Real
 * (monotonic) seconds, deliberately NOT pet-time and NOT scaled by KFDBG
 * MULT's time multiplier: NVS wear is a property of real elapsed time, not
 * simulated pet-time, so this reads kf_time_mono_us() directly rather than
 * riding the same multiplied delta kf_pet_session_frame()'s caller feeds
 * the decay math (see that function's own header comment on why the
 * multiplier only ever reaches the pet's own advance).
 *
 * A periodic tick only actually WRITES if something changed since the last
 * save (a dirty flag, private to kf_pet_session.cpp) -- an idle pet (the
 * concrete case: a dead one, whose kf_pet_advance() is a verified no-op,
 * hakoniwaos/src/pet.cpp's `if (state->dead) return;`) burns no further
 * NVS writes just because the clock ticked. 600 (10 minutes) and the
 * endurance arithmetic behind it are in docs/architecture/adr-0056-
 * pet-save-checkpoints.md -- change this constant and that ADR's numbers
 * both, in the same commit. */
#define KF_PET_SESSION_PERIODIC_SAVE_SECONDS 600u

/* Read-only view of the live pet -- for the Lua binding (ADR 0016), and
 * later a UI screen. Never NULL after kf_pet_session_init(); asserts if
 * called before it, the same "host wiring order, not user input" contract
 * every other *_init()-gated accessor in this codebase uses. */
const kf_pet_state *kf_pet_session_state(void);

/* Care actions against the live pet. See kf/pet.h's kf_pet_feed() etc. --
 * these are exactly those functions, called against the one instance this
 * module owns, `variation` passed straight through. */
void kf_pet_session_feed(uint8_t variation);
void kf_pet_session_play(uint8_t variation);
void kf_pet_session_rest(uint8_t variation);
void kf_pet_session_bath(uint8_t variation);

/* Clears the waiting poops. No variation: see kf/pet.h's kf_pet_flush(). */
void kf_pet_session_flush(void);

/* Wakes the active pet deliberately, if it is currently asleep -- see kf/
 * pet.h's kf_pet_wake(). A no-op otherwise (already awake, or dead), same
 * as every other care action against a dead pet. Task 7 of docs/
 * superpowers/plans/2026-08-13-screens-clock-sleep.md. */
void kf_pet_session_wake(void);

/* Tucks the active pet in, if it is currently drowsy -- see kf/pet.h's
 * kf_pet_tuck_in(). A no-op otherwise (not drowsy: already asleep, dead,
 * an egg, or simply not within the ten-minute window), same "the Core
 * function's own gate is the only gate" shape kf_pet_session_wake() already
 * has. ADR 0052, the 2026-08-11 bedtime-behaviour extension. */
void kf_pet_session_tuck_in(void);

/* The attention signal (Task 8, docs/superpowers/plans/2026-08-13-screens-
 * clock-sleep.md): wraps kf/pet.h's pure kf_pet_wants() with the one piece
 * of memory hysteresis needs -- what this session reported last time --
 * held here rather than in kf_pet_state, so it never touches the save
 * format (see kf_pet_wants()'s own header comment on why). Reset to
 * KF_PET_WANT_NONE by kf_pet_session_init() and by the two DEBUG-gated
 * functions below that fabricate a fresh state (_debug_reset(),
 * _debug_jump_to_stage()) -- a fresh session or a fabricated jump has no
 * real "was already asking" history to carry forward, the same reasoning
 * those two already apply to the debug snapshot ring. */
kf_pet_want kf_pet_session_wants(void);

/* Persists the active pet's state now, via kf_pet_save(). Exposed here for
 * manual checkpoints (the SDL debug window's Save button, and Lua's
 * pet.save() binding), but -- since docs/architecture/adr-0056-pet-save-
 * checkpoints.md -- also called AUTOMATICALLY at every real checkpoint a
 * battery-powered device actually has, because kf_pet_session_shutdown()
 * alone is not one: on ESP32 the main loop's `for(;;)` has no quit
 * condition (ports/esp32/main/app_main.cpp's own comment calls its
 * shutdown call "unreachable in practice"), so a device that is unplugged
 * or battery-dies mid-session never reaches it. The automatic checkpoints,
 * all inside kf_pet_session.cpp:
 *
 *   1. After every care action (feed/play/rest/bath/flush/wake/tuck_in) --
 *      rare, human-paced events, always worth an immediate write.
 *   2. At the awake -> asleep transition, detected live inside
 *      kf_pet_session_frame() -- Chris's "low power mode": the pet is
 *      about to do nothing for hours, so this is the natural checkpoint,
 *      not device deep sleep (kf_power_deep_sleep_until() has no
 *      production caller yet -- see that function's own header comment --
 *      and this task deliberately does not add one).
 *   3. Periodically while running (KF_PET_SESSION_PERIODIC_SAVE_SECONDS),
 *      but ONLY if a private dirty flag says something changed since the
 *      last save -- see that macro's own comment for the endurance
 *      reasoning and where the idle/no-write case actually occurs.
 *
 * Every automatic checkpoint funnels through this same function, so a
 * manual call and an automatic one are indistinguishable and both reset
 * the dirty flag and the periodic timer -- calling this yourself never
 * causes a redundant write shortly after. A failed save is logged, not
 * fatal -- a full NVS partition (KF_ERR_EXHAUSTED) should not crash the
 * device, it should be visible in the log and tried again next
 * checkpoint (the dirty flag is deliberately left set on failure so a
 * later checkpoint keeps retrying, rather than only the player's next
 * care action). */
void kf_pet_session_save(void);

/* Saves, then tears the session down. */
void kf_pet_session_shutdown(void);

/* ---------------------------------------------------------------------
 * DEBUG ONLY below this line. Not part of the gameplay surface (not
 * exposed to Lua -- a script has no business fast-forwarding or resetting
 * time on its own pet) and not something a real device would ever expose
 * to a player.
 *
 * Split across TWO flags, not one, because the six functions below have
 * very different costs:
 *
 *   KF_PET_SESSION_ENABLE_DEBUG_CONTROLS gates kf_pet_session_debug_
 *   advance()/_reset()/_age_seconds()/_jump_to_stage() below -- each a
 *   thin wrapper over kf_pet_advance()/kf_pet_init(), which the gameplay
 *   path already links in; no extra static memory of its own. Cheap
 *   enough that the ESP32 build turns this ON: it is how ports/esp32/
 *   main/kf_dbg_bridge.cpp's KFDBG ADVANCE/RESET/MULT/JUMP commands reach
 *   the pet session at all (see that file, and ADR 0030/0031/0034),
 *   letting a developer fast-forward a real device's four-real-day decay
 *   curve, or jump straight to a life stage's art, over a serial link
 *   instead of watching it do nothing for four days or nursing a pet
 *   through every real stage. _jump_to_stage() rides this flag rather than
 *   a new one because it costs exactly what its three siblings already
 *   cost: one kf_pet_init() call and a few field writes, nothing this
 *   flag's "cheap" half was not already paying for.
 *
 *   KF_PET_SESSION_ENABLE_DEBUG_TOOLS gates kf_pet_session_debug_seek()
 *   below and the scrubbable-timeline snapshot ring backing it -- the
 *   genuinely expensive part, kDebugSnapshotCapacity times sizeof
 *   (DebugSnapshot) north of 200KB of unconditional static memory (see
 *   kf_pet_session.cpp's top-of-file comment). A real device has neither
 *   the SRAM to spare for that nor, so far, a caller for it, so ports/
 *   esp32/main/CMakeLists.txt turns this OFF -- unchanged from before
 *   this split. Its only current callers are desktop-only: sdl_main.cpp's
 *   debug key bindings and sdl_debug_window.cpp's draggable timeline,
 *   added specifically because kf_pet_advance()'s bounded-loop design
 *   (ADR 0021) makes jumping an arbitrary amount of pet-time cheap and
 *   safe to call directly, and the default illustrative stage durations
 *   (an hour for an egg, about a week for a full grow-up) are otherwise
 *   much too slow to watch interactively. The same flag also gates
 *   kf_pet_session_state_mutable_for_test() further down (Task 5) -- cheap
 *   in itself, but a test-only reach-into-Core hazard a real device has no
 *   business linking in any more than it does the snapshot ring, so it
 *   rides the same flag rather than getting a third one of its own.
 *
 * All six are DECLARED unconditionally below, same as everything else
 * in this header, so nothing calling into this file needs to know or
 * care which backend it is, or which of the two flags gates which
 * function below. Whether each is DEFINED depends on its own flag --
 * see kf_pet_session.cpp's top-of-file comment. Desktop/headless get real
 * definitions of all six by default. Calling a function whose flag is
 * off for the calling backend is therefore a link error, not
 * silently-wrong behaviour -- the correct outcome, since a function
 * gated off is not meant to be reachable from that backend.
 * --------------------------------------------------------------------- */

/* Advances the live pet by exactly `seconds`, immediately -- bypassing
 * kf_pet_session_frame()'s live-tick batching (KF_PET_SESSION_FLUSH_
 * SECONDS) entirely, since that batching exists to make small per-frame
 * deltas add up correctly over real time, not to gate a deliberate,
 * one-shot jump. Uses the exact same kf_pet_advance() offline fast-forward
 * relies on, so this is not a separate, less-tested code path -- it is
 * the same one, called on demand instead of at boot.
 *
 * MOVES THE WALL CLOCK BY THE SAME `seconds` TOO, since 2026-08-11. It did
 * not before, which was harmless while nothing read the hour and quietly
 * wrong once sleep landed: Core's clock and kf_time_wall() -- what Lua's
 * kf.hour(), the home screen's wall clock and the drowsy window all read --
 * would drift apart by exactly however much was skipped, so skipping a week
 * left a creature that would not go to bed at a bedtime the screen was
 * plainly showing. Contrast kf_pet_session_debug_set_clock() below, which
 * TELEPORTS both clocks without ageing the pet; this one ages the pet by
 * the distance travelled, which is what "skip a week" should mean.
 *
 * On device this genuinely moves the DS3231, and the move survives a power
 * cut -- see the implementation's own comment. Debug-gated, but not a
 * display-only fast-forward.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS -- see this section's
 * header comment. Reachable on ESP32 via KFDBG ADVANCE (ADR 0030). */
void kf_pet_session_debug_advance(uint32_t seconds);

/* The three points in the sleep cycle the desktop debug window's
 * Drowsy/Bedtime/Morning buttons jump to.
 *
 * DEFINED HERE, NOT IN THE DEBUG WINDOW, because the headless check has to
 * assert against the times the buttons ACTUALLY use. The first version of
 * these buttons got this wrong in a way worth recording: the buttons aimed
 * ten seconds SHORT of each transition (21:59:50, 06:59:50) so the change
 * could be watched happening, while clock_jump_check tested times ten
 * seconds PAST them (22:00:30, 07:00:30). Every assertion passed and the
 * buttons still did nothing visible, because the check was testing the
 * function rather than the buttons. One definition, both callers. */
typedef enum {
    KF_PET_DEBUG_CLOCK_DROWSY = 0,  /* 21:50:05 -- inside the ten-minute
                                     * tuck-in window (ADR 0052) */
    KF_PET_DEBUG_CLOCK_BEDTIME,     /* 22:00:05 -- inside the night window */
    KF_PET_DEBUG_CLOCK_MORNING,     /* 07:00:05 -- past the end of it */
} kf_pet_debug_clock_point;

/* The next epoch second at which the local clock reads `point`'s time of
 * day. If that moment has already passed today, tomorrow's -- never a jump
 * backwards, which Core's night accounting is not written for and
 * kf_pet_advance() is not defined for.
 *
 * EVERY POINT LANDS INSIDE THE STATE IT NAMES, five seconds past the
 * boundary rather than short of it, so pressing a button changes what is on
 * screen immediately. Aiming short does not work here and the reason is not
 * obvious: kf_pet_session_frame() only re-evaluates the pet every
 * KF_PET_SESSION_FLUSH_SECONDS (30) of pet time, so a target ten seconds
 * before a transition leaves up to thirty more seconds of apparently
 * nothing happening -- which reads as a broken button. Landing inside the
 * window instead means kf_pet_session_debug_set_clock()'s own one-second
 * advance settles the new state on the press itself.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS. Reachable on ESP32 too, via
 * KFDBG CLOCK DROWSY/BEDTIME/MORNING (kf_dbg_bridge.cpp's
 * handle_clock_point()) -- the hardware debug bridge's equivalent of these
 * desktop buttons, added so the two surfaces stay at parity. */
int64_t kf_pet_session_debug_clock_target(kf_pet_debug_clock_point point);

/* Moves the WHOLE WORLD's clock to `epoch_seconds` without ageing the pet
 * by the distance travelled. Built for testing the night window (ADR 0048):
 * "jump to just before bedtime and watch it fall asleep" is otherwise a
 * ten-hour wait or a ten-hour fast-forward that starves the creature on the
 * way, and a starving pet is noise when the thing under test is the clock.
 *
 * TWO CLOCKS HAVE TO MOVE TOGETHER, and this is the whole reason this
 * function exists rather than the caller just calling kf_time_set_wall():
 *
 *   1. The HAL wall clock (kf_time_set_wall()), which is what Lua's
 *      kf.hour() reads, and therefore what creature.lua's drowsy cue and
 *      clock display are driven by.
 *   2. kf_pet_state::last_advanced, which is what CORE evaluates the
 *      22:00-07:00 window against (apply_stage_segment(), pet.cpp).
 *
 * They are NOT the same clock and nothing keeps them in step by itself.
 * Wall time enters Core exactly once, at load (kf_pet_load_and_advance());
 * after that kf_pet_session_frame() ticks the pet from the MONOTONIC clock
 * and kf_pet_advance() carries last_advanced forward by that delta (Task 6,
 * ADR 0048). So setting only the wall clock would leave Lua showing 22:00
 * while Core still believed it was mid-afternoon, and the creature would
 * stay stubbornly awake -- which looks exactly like a broken night window
 * and is not one. Setting both is also self-correcting: it re-syncs the two
 * if kf_pet_session_debug_advance() (which deliberately moves Core's clock
 * and NOT the wall clock) has already pulled them apart.
 *
 * Needs are untouched, so this is NOT "skip ahead ten hours" -- it is
 * "pretend it was always this time". A one-second advance is applied
 * afterwards purely so Core re-evaluates `asleep` immediately rather than
 * on the next flush, since sleep is only recomputed inside an advance.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS -- see this section's
 * header comment. Reachable on ESP32 via KFDBG CLOCK (see
 * kf_dbg_bridge.cpp's handle_clock_point()/handle_clock_epoch()), added
 * alongside kf_pet_session_debug_clock_target() below to close the gap
 * between the desktop debug window's Drowsy/Bedtime/Morning buttons and the
 * hardware debug bridge -- see this file's own callers list for the
 * desktop side, which predates the KFDBG verb and is unchanged by it. */
void kf_pet_session_debug_set_clock(int64_t epoch_seconds);

/* Resets the live pet to a fresh egg, in place -- without touching
 * whatever is currently on disk. The next normal checkpoint (a care
 * action, or shutdown) overwrites the save with this fresh state. Lets
 * someone testing branch outcomes start over without restarting the
 * whole process or deleting a save file by hand. Also clears the
 * snapshot history kf_pet_session_debug_seek() reads (see below), on
 * backends where that history exists at all -- a fresh egg starts a
 * genuinely new timeline; the previous pet's history has nothing to do
 * with it.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS -- see this section's
 * header comment. Reachable on ESP32 via KFDBG RESET (ADR 0030). */
void kf_pet_session_debug_reset(void);

/* Jumps the live pet directly to the START of `stage` -- alive, not sick,
 * not dead, every need topped up to KF_PET_MILLIPERCENT_MAX, exactly as
 * kf_pet_session_debug_reset() leaves a fresh egg, except landing on
 * whichever stage was asked for instead of always Egg. Built for looking
 * at a life stage's art and care behaviour (Chris: "spawns the creature
 * at full care stats... and timeline moves to the beginning of each life
 * stage") without nursing a pet through every real stage first.
 *
 * Implemented as kf_pet_init() followed by setting `state.stage` directly,
 * NOT as kf_pet_advance() walked forward through however many real
 * stages -- and that is a design decision, not a shortcut. Every input
 * kf_pet_advance()'s own branch selection would use (care_actions_taken,
 * care_integral_mp_seconds) would be exactly as fabricated as a hand-set
 * field if this function drove them, just dressed up as though a pet had
 * really been raised. THE JUMPED-TO PET HAS NO REAL CARE HISTORY: its
 * `care_actions_taken` reads 0 (kf_pet_init()'s own default) and its
 * teen_form/adult_branch are never derived from any care average at all --
 * they are set directly from `teen_form`/`adult_branch` below, or default
 * to 0 (the first form) if those are out of range. Branch selection that
 * ran off a jump's fabricated history would not be meaningless in a
 * subtle way -- it would be *wrong* in a specific, checkable way (an
 * untouched creature's care_actions_taken == 0 always drops it into
 * KF_PET_TEEN_FORM_DUST, see advance_to_next_stage() in kf/pet.cpp), which
 * is exactly why this function does not attempt it and hands the choice to
 * the caller instead.
 *
 * `teen_form`/`adult_branch` are meaningless before their own branch point
 * (kf/pet.h's own words) and are therefore only written when `stage` has
 * passed it: `teen_form` for stage >= KF_PET_STAGE_TEEN, `adult_branch`
 * for stage == KF_PET_STAGE_ADULT (against kf_pet_adults_in_family() of
 * whichever `teen_form` was just set, since families do not all have the
 * same adult count -- see kf/pet.h). Out-of-range input in either -- not
 * just unset -- falls back to 0 rather than being written raw: 0 is always
 * a valid family index, and every family has at least one adult, so 0 is
 * always a valid adult index too, which makes "unset" and "invalid" the
 * same safe case instead of two to get right separately. `teen_form`'s
 * valid range is [0, KF_PET_TEEN_FORM_DUST] -- the dust form is a real,
 * reachable form (see the paragraph above), not an error value, so it is
 * accepted here exactly like any of the four verb families; only input
 * past it falls back to 0. `stage` itself is clamped to KF_PET_STAGE_ADULT
 * if it somehow arrives out of range.
 *
 * Also clears the snapshot history (see kf_pet_session_debug_reset()'s own
 * comment on why): a jump is a fabricated state with no continuity from
 * whatever the pet was doing a moment before, not a new data point on the
 * same timeline.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS -- see this section's
 * header comment. Reachable on ESP32 via KFDBG JUMP (see
 * kf_dbg_bridge.cpp's handle_jump()); desktop-only caller is
 * sdl_debug_window.cpp's stage-jump buttons. */
void kf_pet_session_debug_jump_to_stage(kf_pet_stage stage, uint8_t teen_form,
                                         uint8_t adult_branch);

/* Total elapsed pet-age in seconds since this pet's own genesis (the
 * last kf_pet_session_init() or kf_pet_session_debug_reset()) --
 * cumulative stage durations already lived through, plus the current
 * stage's own state->stage_elapsed_seconds. This is the pet's own
 * lifetime clock, not a wall-clock or session-uptime reading: two eggs
 * hatched a real week apart, both still 10 minutes into being a baby,
 * report the same age. The debug window's timeline (sdl_debug_window.cpp)
 * uses this as its X axis.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_CONTROLS -- see this section's
 * header comment. Reachable on ESP32 via KFDBG STATE's pet_age_s field
 * (ADR 0030). */
uint64_t kf_pet_session_debug_age_seconds(void);

/* Scrubs the live pet to exactly `target_age_seconds` on its own
 * lifetime clock (see kf_pet_session_debug_age_seconds() above) --
 * forward OR backward, for the debug window's draggable timeline.
 *
 * Backing this is a bounded ring buffer of full kf_pet_state snapshots,
 * taken automatically after every state change (a live-tick flush, a
 * debug_advance(), a care action, or a reset) -- NOT an action replay
 * log. A seek finds the most recent snapshot at or before the target
 * age and, if the target lands after it, calls kf_pet_advance() for the
 * remainder -- the same closed-form call kf_pet_session_debug_advance()
 * already uses, so hunger/happiness/energy and exactly WHEN each stage
 * transition happens are always exactly right, not approximated, no
 * matter how far the seek jumps.
 *
 * One documented inexactness, inherited rather than introduced: WHICH
 * teen_form/adult_branch a seek lands on can occasionally differ by one
 * band from what continuous live play would have picked for the same
 * care history, because care_integral_mp_seconds is itself a
 * left-Riemann approximation (see kf/pet.cpp's file header) whose error
 * depends on how finely the elapsed time happens to get chunked -- a
 * seek's chunking (by snapshot spacing) is not guaranteed to match
 * whatever chunking the original live session used. This is the same
 * category of approximation kf_pet_session_frame()'s own live-tick
 * batching already accepts, not a new one, and it never affects hunger/
 * happiness/energy or stage TIMING, only which sibling branch gets
 * picked right at a stage transition.
 *
 * Snapshot history does not survive kf_pet_session_debug_reset() (see
 * above) and is bounded (oldest snapshots are evicted once the ring
 * fills) -- seeking earlier than the oldest surviving snapshot clamps
 * to that snapshot instead of erroring. If you scrub backward and then
 * resume different play than originally happened, the ring may end up
 * holding snapshots from both the original and the new timeline in the
 * same age range; a later seek into that range picks whichever
 * happens to still be in the ring, not a real branch history -- this is
 * a preview tool, not a save-state manager, and does not attempt to
 * track timeline branches.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_TOOLS -- the expensive one; see
 * this section's header comment. Desktop/headless only, unchanged: not
 * reachable on ESP32. */
void kf_pet_session_debug_seek(uint64_t target_age_seconds);

/* Mutable access to the live pet, for tests that need to force a field no
 * care action or debug lever reaches directly -- e.g. poop_count, to drive
 * the creature screen's mess-drawing budget without playing out real
 * neglect. This is NOT a second way for presentation to influence Core: the
 * shipping path (kf_creature_screen.cpp and friends) reads only the const
 * pointer kf_pet_session_state() returns, never this one, and this
 * declaration exists solely so a test file can reach in.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_TOOLS, same as kf_pet_session_debug_
 * seek() just above -- see this section's header comment. Desktop/headless
 * only: not reachable on ESP32, exactly like the other TOOLS-gated
 * functions here. */
kf_pet_state *kf_pet_session_state_mutable_for_test(void);

/* TEST ONLY: makes the periodic safety-net checkpoint (KF_PET_SESSION_
 * PERIODIC_SAVE_SECONDS, kf_pet_session_save()'s own comment) immediately
 * due, without waiting that many REAL monotonic seconds -- unlike the
 * pet's own simulated clock (kf_time_set_wall() and friends), this
 * project has no way to fast-forward kf_time_mono_us() itself (it is the
 * host's real steady_clock, see kf/hal/time.h), so a test that wants to
 * observe the periodic checkpoint firing needs this rather than an actual
 * ten-minute sleep. Does not touch the dirty flag -- a periodic save
 * still only actually writes if something changed since the last one, so
 * this proves the TIMER path, not a way to force a write past the dirty
 * check.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_TOOLS, same as every other test-only
 * reach into this file's session state above. Desktop/headless only: not
 * reachable on ESP32. */
void kf_pet_session_debug_force_periodic_save_due(void);

/* TEST ONLY: how many times kf_pet_session_save() has actually invoked
 * kf_pet_save() (i.e. attempted a real write to the store) since
 * kf_pet_session_init() -- whether that write succeeded or not, and
 * regardless of which checkpoint triggered it (manual, care-action,
 * sleep-transition, or periodic). Behavioural proof that a given call DID
 * or DID NOT reach the store, which reading kf_pet_session_state() alone
 * cannot give you: a no-op care action (e.g. feeding a dead pet) and a
 * real one that happens to leave every field unchanged look identical in
 * the state, but not in whether a write actually happened.
 *
 * Gated by KF_PET_SESSION_ENABLE_DEBUG_TOOLS, same as every other test-only
 * reach into this file's session state above. Desktop/headless only: not
 * reachable on ESP32. */
uint32_t kf_pet_session_debug_save_attempt_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_PET_SESSION_H */
