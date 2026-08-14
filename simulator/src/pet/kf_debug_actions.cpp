/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * The one table of portable debug actions. See kf_debug_actions.h for what
 * belongs in it, what deliberately does not, and why this exists at all.
 *
 * Every handler here is a thin adapter onto kf_pet_session.h or
 * kf_screen_nav.h -- this file owns the TABLE, not the behaviour. If a
 * handler below grows real logic, that logic belongs in the session layer
 * where both the debug path and the real path can reach it, not here where
 * only the debug path can.
 */

#include "kf_debug_actions.h"

#include "kf_pet_session.h"
#include "kf_screen_nav.h"
#include "kf/pet.h"

#include <cstring>

namespace {

void run_advance(const kf_debug_args *args) {
    kf_pet_session_debug_advance(args->value);
}

void run_reset(const kf_debug_args *args) {
    (void)args;
    kf_pet_session_debug_reset();
}

void run_save(const kf_debug_args *args) {
    (void)args;
    kf_pet_session_save();
}

void run_screen(const kf_debug_args *args) {
    (void)args;
    /* kf_screen_nav_debug_advance(), NOT an injected KF_BTN_MENU edge. The
     * two are deliberately different: a real MENU press also toggles Core's
     * on-device HUD (kf/app.cpp), and this action exists to change screen
     * WITHOUT that side effect. Both remain available -- the bridge still
     * has BTN for the real-edge version. */
    kf_screen_nav_debug_advance();
}

/* teen_form/adult_branch arrive as uint32_t because that is what a decimal
 * token parses into, and narrow to uint8_t at the session boundary. Explicit
 * casts, not implicit: the session validates the VALUES itself (see
 * kf_pet_session_debug_jump_to_stage()), so silently truncating a
 * nonsense 300 to 44 here would hand it something it would then treat as
 * plausible. Clamped rather than truncated for that reason. */
uint8_t narrow_index(uint32_t v) {
    return static_cast<uint8_t>(v > 255u ? 255u : v);
}

void run_jump(const kf_debug_args *args) {
    kf_pet_session_debug_jump_to_stage(static_cast<kf_pet_stage>(args->value),
                                        narrow_index(args->teen_form),
                                        narrow_index(args->adult_branch));
}

void run_next_stage(const kf_debug_args *args) {
    /* One press, one stage forward from wherever the pet currently is.
     * Adult is terminal (kf/pet.h), so once already there "the following
     * stage" is Adult itself: this still refills needs and clears
     * sickness/neglect/mess, because kf_pet_session_debug_jump_to_stage()
     * always starts from kf_pet_init(); it just does not move the marker
     * further along a timeline that has nowhere further to go. */
    const kf_pet_stage current = kf_pet_session_state()->stage;
    const kf_pet_stage next =
        current < KF_PET_STAGE_ADULT
            ? static_cast<kf_pet_stage>(static_cast<int>(current) + 1)
            : KF_PET_STAGE_ADULT;
    kf_pet_session_debug_jump_to_stage(next, narrow_index(args->teen_form),
                                        narrow_index(args->adult_branch));
}

void run_clock(const kf_debug_args *args) {
    switch (args->clock) {
    case KF_DEBUG_CLOCK_DROWSY:
        kf_pet_session_debug_set_clock(
            kf_pet_session_debug_clock_target(KF_PET_DEBUG_CLOCK_DROWSY));
        break;
    case KF_DEBUG_CLOCK_BEDTIME:
        kf_pet_session_debug_set_clock(
            kf_pet_session_debug_clock_target(KF_PET_DEBUG_CLOCK_BEDTIME));
        break;
    case KF_DEBUG_CLOCK_MORNING:
        kf_pet_session_debug_set_clock(
            kf_pet_session_debug_clock_target(KF_PET_DEBUG_CLOCK_MORNING));
        break;
    case KF_DEBUG_CLOCK_SYNC_HOST:
    case KF_DEBUG_CLOCK_EPOCH:
    default:
        /* Both land here: "sync to host" is resolved to an absolute epoch by
         * the CALLER, because only the desktop has a host clock to read and
         * kf_host_time_system_now() is not part of the device build. The
         * device reaches the same place with KFDBG CLOCK EPOCH <n>, which is
         * what tools/kf_debug.py's `clock sync` sends after reading the
         * host clock itself. One handler, two ways of deciding the number. */
        kf_pet_session_debug_set_clock(args->epoch_seconds);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * THE TABLE.
 *
 * Order is the order KFDBG HELP and the desktop listing print in, so it is
 * grouped by what a person is trying to do rather than alphabetically:
 * time first, then lifecycle, then everything else.
 * ------------------------------------------------------------------------- */
const kf_debug_action kActions[] = {
    {"ADVANCE", KF_DEBUG_ARG_U32, true,
     "advance pet time by N seconds", run_advance},
    {"CLOCK", KF_DEBUG_ARG_CLOCK, true,
     "move the wall clock to drowsy, bedtime, morning or an epoch",
     run_clock},
    {"JUMP", KF_DEBUG_ARG_STAGE, true,
     "jump to a stage (0=egg..4=adult), optionally naming form and branch",
     run_jump},
    {"NEXTSTAGE", KF_DEBUG_ARG_NONE, true,
     "jump to the start of the stage after the current one", run_next_stage},
    {"RESET", KF_DEBUG_ARG_NONE, true, "reset to a fresh egg", run_reset},
    {"SAVE", KF_DEBUG_ARG_NONE, true, "force a save checkpoint now",
     run_save},
    {"SCREEN", KF_DEBUG_ARG_NONE, true,
     "advance to the next screen without a MENU edge", run_screen},
};

/* The four verbs that read hardware a desktop does not have. Kept as data,
 * not just prose, so the parity test can assert that every KFDBG verb is
 * either in kActions or declared here -- see kf_debug_actions.h. */
const char *const kDeviceOnlyVerbs[] = {
    "RTC",
    "SCANLINE",
    "VSYNC",
    "SHOT",
};

/* Verbs that exist on both sides through different mechanisms, so their
 * absence from kActions is not a gap. See kf_debug_actions.h for the
 * reasoning on each. */
const char *const kOtherMeansVerbs[] = {
    "FEED", "PLAY", "REST", "BATH", "FLUSH", /* real on-screen care buttons */
    "BTN",  "BTNHOLD",                       /* a real keyboard */
    "PING",                                  /* serial liveness */
    "MULT",                                  /* per-backend holder, by design */
    "STATE", /* the desktop window shows this continuously, not on request */
};

} // namespace

const kf_debug_action *kf_debug_actions(size_t *count_out) {
    if (count_out != nullptr) {
        *count_out = sizeof(kActions) / sizeof(kActions[0]);
    }
    return kActions;
}

const kf_debug_action *kf_debug_action_find(const char *verb) {
    if (verb == nullptr) {
        return nullptr;
    }
    const size_t n = sizeof(kActions) / sizeof(kActions[0]);
    for (size_t i = 0; i < n; ++i) {
        if (std::strcmp(kActions[i].verb, verb) == 0) {
            return &kActions[i];
        }
    }
    return nullptr;
}

const char *const *kf_debug_actions_device_only_verbs(size_t *count_out) {
    if (count_out != nullptr) {
        *count_out = sizeof(kDeviceOnlyVerbs) / sizeof(kDeviceOnlyVerbs[0]);
    }
    return kDeviceOnlyVerbs;
}

const char *const *kf_debug_actions_other_means_verbs(size_t *count_out) {
    if (count_out != nullptr) {
        *count_out = sizeof(kOtherMeansVerbs) / sizeof(kOtherMeansVerbs[0]);
    }
    return kOtherMeansVerbs;
}
