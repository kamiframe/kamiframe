/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * One table of debug actions, driven by both the desktop debug window and the
 * KFDBG serial bridge, so a debug capability exists on both sides or neither.
 *
 * ============================================================================
 *  WHY THIS EXISTS
 *
 *  The underlying debug operations were already shared -- kf_pet_session.h's
 *  kf_pet_session_debug_*() family is compiled into both builds, and both
 *  callers already went through it. What was NOT shared was the two
 *  DISPATCHERS on top: an if/else chain of verb strings in
 *  ports/esp32/main/kf_dbg_bridge.cpp, and a button array in
 *  simulator/src/sdl/sdl_debug_window.cpp. Two hand-maintained lists over one
 *  shared API, with nothing connecting them.
 *
 *  So adding a debug capability meant remembering to edit both, and the
 *  failure mode when you didn't was silence: the feature simply wasn't there
 *  on the other side, with no build error and no failing test. That gap had
 *  already opened twice by the time this file was written -- "Save Now" and
 *  "jump to next stage" both existed as desktop buttons with no KFDBG verb.
 *
 *  This is ADR 0058's fix applied one layer up. That one merged two
 *  hand-maintained copies of the per-frame sequence after they drifted into a
 *  real hardware bug; this one merges two hand-maintained copies of the debug
 *  surface before they can.
 * ============================================================================
 *
 * WHAT IS IN THIS TABLE, AND WHAT DELIBERATELY IS NOT.
 *
 * PORTABLE actions only -- the ones implementable purely in terms of code
 * both builds compile. Those are the ones that can drift, because they are
 * the ones both sides could have.
 *
 * Four KFDBG verbs stay in kf_dbg_bridge.cpp and are NOT listed here, because
 * they read hardware a desktop does not have and never will:
 *
 *   RTC        reads the DS3231 over I2C
 *   SCANLINE   reads the panel's own scanline counter over SPI
 *   VSYNC      times that counter's wrap
 *   SHOT       ships a framebuffer over the serial link (the desktop has a
 *              window; there is nothing to ship it to)
 *
 * Their absence from the desktop is not drift and closing it is not a goal.
 * A "parity" check that demanded a desktop RTC button would be demanding a
 * lie. See kf_debug_actions_device_only_verbs() for the same list in a form a
 * test can read, so that this reasoning is enforced rather than merely
 * written down.
 *
 * Three more debug affordances are parity BY DIFFERENT MEANS, and are also
 * absent here on purpose:
 *
 *   care actions   FEED/PLAY/REST/BATH/FLUSH are KFDBG verbs on device; on
 *                  desktop you press the real on-screen care buttons, which
 *                  exercises more of the stack, not less.
 *   button inject  BTN/BTNHOLD on device; on desktop, an actual keyboard.
 *   PING           transport liveness for a serial link that the desktop
 *                  does not have.
 *   MULT           both sides have it, so it is not a gap -- but the HOLDER
 *                  is per-backend on purpose, and kf_frame_loop.h says so
 *                  explicitly: each bridge owns its own multiplier and
 *                  passes it to kf_frame_loop_run(). Pulling that into this
 *                  table would mean overturning a decision this file has no
 *                  business overturning as a side effect of tidying.
 */

#ifndef KF_DEBUG_ACTIONS_H
#define KF_DEBUG_ACTIONS_H

#include <cstddef>
#include <cstdint>

/* What a caller must parse and supply before invoking an action. The KFDBG
 * bridge reads this to know how many tokens to pull off the line; the desktop
 * window reads it to know which fields of kf_debug_args its button binding
 * has to fill in. */
typedef enum {
    KF_DEBUG_ARG_NONE,  /* no arguments at all */
    KF_DEBUG_ARG_U32,   /* one unsigned value: ADVANCE <seconds> */
    KF_DEBUG_ARG_STAGE, /* stage, plus optional teen_form and adult_branch */
    /* CLOCK's own shape: one of three named targets, or EPOCH plus an
     * absolute time. Given its own kind rather than being split into four
     * verbs because "KFDBG CLOCK BEDTIME" is the spelling already shipped
     * and documented (ADR 0054), and a parity mechanism that renames the
     * commands it is unifying is a migration, not a parity fix. */
    KF_DEBUG_ARG_CLOCK,
} kf_debug_arg_kind;

/* Which time KF_DEBUG_ARG_CLOCK moves to. The three named ones resolve
 * through kf_pet_session_debug_clock_target() rather than naming times at
 * any call site -- the first version of the desktop buttons named them
 * inline, aimed ten seconds SHORT of each transition, and appeared to do
 * nothing against the 30-second session flush. See that function. */
typedef enum {
    KF_DEBUG_CLOCK_DROWSY,
    KF_DEBUG_CLOCK_BEDTIME,
    KF_DEBUG_CLOCK_MORNING,
    KF_DEBUG_CLOCK_EPOCH,     /* absolute, from kf_debug_args::epoch_seconds */
    KF_DEBUG_CLOCK_SYNC_HOST, /* desktop-only in practice; see the table */
} kf_debug_clock_target;

/* Every argument any action can take, in one struct rather than a union or an
 * overload set. Three uint32_t is small enough that saving two of them is not
 * worth a variant type in a table this size, and a plain struct keeps the
 * handler signature identical for every entry -- which is what lets the table
 * hold plain function pointers and stay heap-free. Fields an action does not
 * use are ignored, not validated. */
typedef struct {
    uint32_t value;        /* KF_DEBUG_ARG_U32, and the stage for _STAGE */
    uint32_t teen_form;    /* KF_DEBUG_ARG_STAGE only; defaults to 0 */
    uint32_t adult_branch; /* KF_DEBUG_ARG_STAGE only; defaults to 0 */
    kf_debug_clock_target clock;   /* KF_DEBUG_ARG_CLOCK only */
    int64_t epoch_seconds;         /* KF_DEBUG_CLOCK_EPOCH only */
} kf_debug_args;

typedef struct {
    /* The KFDBG verb, uppercase, no "KFDBG " prefix. Also the key the
     * desktop button table binds against, so these strings are the one
     * spelling of an action's identity across both sides. */
    const char *verb;

    kf_debug_arg_kind arg;

    /* Mutate tier (ADR 0035) -- true means the action changes pet or clock
     * state and the bridge must refuse it unless KF_DBG_MUTATE_ENABLE is on.
     * Kept here rather than at the call site so the two dispatchers cannot
     * disagree about which actions are dangerous. */
    bool mutates;

    /* One line, shown by KFDBG HELP and by the desktop window's own listing.
     * Present tense, no trailing period. */
    const char *summary;

    void (*run)(const kf_debug_args *args);
} kf_debug_action;

/* The table itself. Returns the base pointer and writes the entry count.
 * Static storage with static lifetime -- no allocation, safe to hold. */
const kf_debug_action *kf_debug_actions(size_t *count_out);

/* Verb lookup, case-sensitive (KFDBG verbs are uppercase by convention and
 * the bridge already upper-cases nothing -- see kf_dbg_bridge.cpp's own
 * parser). Returns nullptr when no action has that verb, which is what the
 * bridge turns into its "unknown subcommand" reply. */
const kf_debug_action *kf_debug_action_find(const char *verb);

/* The device-only verbs named in this file's header comment, as data a test
 * can iterate. The parity test uses this to assert that every KFDBG verb is
 * EITHER in the shared table above OR explicitly declared device-only here --
 * so a fifth device-only verb added later cannot quietly become an
 * unnoticed gap. It has to be declared as a deliberate exception, in this
 * file, next to the reasoning for the other four. */
const char *const *kf_debug_actions_device_only_verbs(size_t *count_out);

/* Likewise for the verbs that are parity by different means (care actions,
 * button injection, PING). Same enforcement, different reason -- see the
 * header comment. */
const char *const *kf_debug_actions_other_means_verbs(size_t *count_out);

#endif /* KF_DEBUG_ACTIONS_H */
