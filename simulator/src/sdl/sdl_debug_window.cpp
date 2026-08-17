/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "sdl_debug_window.h"

#include "sdl_shared.h"

#include "../host/host_time.h"
#include "../pet/kf_debug_actions.h"
#include "../pet/kf_pet_session.h"
#include "../pet/kf_screen_nav.h"

#include "kf/app.h"
#include "kf/arena.h"
#include "kf/clock.h"
#include "kf/hal/log.h"
#include "kf/hal/time.h"
#include "kf/pet.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

constexpr const char *TAG = "debug-window";

/* The original layout's own width, unchanged -- every button and the
 * timeline below are still positioned within this, not the wider
 * kWindowW below. Split out so widening the window for the new right-hand
 * diagnostics column (kRightColumnW) does not silently stretch the
 * timeline bar or anything else across the extra space along with it. */
constexpr int kLeftColumnW = 400;
constexpr int kRightColumnW = 260;
constexpr int kWindowW = kLeftColumnW + kRightColumnW;
/* Grew from 460 with the stage-jump controls (Task 8): four new button
 * rows (teen_form picker, adult_branch picker, the five stage-jump
 * buttons, and Next Stage) pushed the timeline and the text readout below
 * them further down the window -- see kTimelineBar and the readout's own
 * starting `y` further down in this file for where. */
/* 670 -> 710 for the sleep-cycle button row (Drowsy/Bedtime/Morning at
 * y=336), which pushed kTimelineBar and the left-hand text readout below it
 * down by one 40px row each. Those three positions move together; changing
 * one without the others overlaps the timeline with either the buttons
 * above it or the readout below. */
/* 710 -> 830 for the neglect/sickness/death diagnostics, including the
 * "last died of" history line (Chris's pet died and this window could not
 * say why -- see the new block at the bottom of the text readout, below
 * hunger/happy/energy). This growth is DIFFERENT
 * in kind from every bump above it: those all pushed kTimelineBar and the
 * readout's own starting `y` DOWN because they added button rows ABOVE the
 * timeline. This one adds lines to the BOTTOM of the readout instead, so
 * kTimelineBar and the readout's starting y (440, below) are UNCHANGED --
 * only the window has to grow, to stop the new lines drawing past its
 * bottom edge. Read this comment before assuming every kWindowH change
 * means moving the timeline too; it only does when the new content sits
 * above it. */
constexpr int kWindowH = 830;
constexpr float kRightColumnX = static_cast<float>(kLeftColumnW) + 16.0f;

enum class DebugAction {
    kSkipHour,
    kSkipDay,
    kSkipWeek,
    kReset,
    kSave,
    kNextScreen,
    kMult1,
    kMult2,
    kMult4,
    kMult8,
    kMult16,
    kMult32,
    kMult64,
    kMult128,
    kMult256,

    /* Task 8: which teen_form/adult_branch a stage-jump button below will
     * hand to kf_pet_session_debug_jump_to_stage() -- see that function's
     * own header comment (kf_pet_session.h) for why the SESSION layer
     * takes concrete indices rather than an "unset" sentinel: this window
     * is where "unset" gets resolved to a concrete default (0, via
     * Session::selected_teen_form/_adult_branch below), the same way
     * g.multiplier already resolves "no multiplier button pressed yet" to
     * 1x rather than passing an optional through to the frame loop. */
    kTeenForm0,
    kTeenForm1,
    kTeenForm2,
    kTeenForm3,

    /* Judgement call (review of Task 8): the dust form
     * (KF_PET_TEEN_FORM_DUST, kf/pet.h -- deliberately equal to
     * KF_PET_TEEN_FORM_COUNT, one past the four verb families above) is a
     * REAL teen_form a genuinely-neglected creature reaches
     * (advance_to_next_stage(), hakoniwaos/src/pet.cpp), not an error
     * value, and this window's whole purpose is making every form
     * inspectable -- a picker that silently could not reach one of them
     * would be exactly the gap this button closes. Named for what it is,
     * not "Form4", since dust is not one of the four families the
     * Form0-3 buttons pick between. */
    kTeenFormDust,

    kAdultBranch0,
    kAdultBranch1,
    kAdultBranch2,

    /* Task 8: jump the live pet directly to the start of a named stage --
     * see kf_pet_session_debug_jump_to_stage()'s own header comment for
     * exactly what "start of a stage" means (alive, full needs, not sick,
     * at stage_elapsed_seconds == 0) and why teen_form/adult_branch above
     * are meaningless (and therefore ignored) for a jump to a stage before
     * their own branch point. */
    kJumpEgg,
    kJumpBaby,
    kJumpChild,
    kJumpTeen,
    kJumpAdult,

    /* Task 8: "I want a button to just autoprogress" (Chris's own words) --
     * jumps from wherever the pet currently is to the START of the
     * following stage, so repeated presses walk egg -> baby -> child ->
     * teen -> adult one press at a time. A no-op-but-for-the-full-refill
     * once already at Adult: Adult is terminal (kf/pet.h), so "the
     * following stage" from Adult is Adult itself -- see perform() below. */
    kJumpNextStage,

    /* Jump the world's clock to a named point in the sleep cycle, so the
     * 22:00-07:00 night window (ADR 0048) is testable without waiting for
     * the real hour to come round. All three go through
     * kf_pet_session_debug_set_clock(), which moves the HAL wall clock and
     * Core's last_advanced TOGETHER -- read that function's header comment
     * in kf_pet_session.h before touching these. Moving only one of the two
     * produces a creature that looks broken and is not.
     *
     * Every target lands five seconds INSIDE the state it names, so a press
     * changes the screen immediately. The times themselves live in
     * kf_pet_session_debug_clock_target(), NOT here -- read its header
     * comment before changing any of this. The first version of this row
     * named the times locally and aimed ten seconds SHORT of each
     * transition, meaning to make the change watchable; it instead made the
     * buttons look broken, because the session only re-evaluates the pet
     * every KF_PET_SESSION_FLUSH_SECONDS (30) of pet time and a ten-second
     * lead is invisible against that. */
    kClockDrowsy,
    kClockBedtime,
    kClockMorning,

    /* Put every clock back to the HOST MACHINE's real time (Chris,
     * 2026-08-12: "a button to reset/sync the hardware clock and in game
     * software clock back to my computer's current time"). The three
     * buttons above, the skip buttons and the speed multiplier all drag the
     * simulated clock away from reality on purpose -- and a testing session
     * drifts it a long way, since each Bedtime press that lands on an
     * already-passed target jumps a whole day forward rather than
     * backwards. This is the way back, and it sets the DATE as well as the
     * time, which nothing else on this window does. */
    kClockSyncHost,
};

struct DebugButton {
    SDL_FRect rect;
    const char *label;
    DebugAction action;
};

/* Laid out by hand, same as kf_pet_screen.cpp's make_row()/make_button() --
 * a handful of fixed rects is not worth a layout engine for either
 * screen. Two multiplier rows (1x-16x, then 32x-256x): Chris's own call
 * that 8x still wasn't fast enough to watch decay happen -- 256x turns
 * one real second into ~4.3 sim minutes, fast enough to watch a whole
 * stage go by in well under a minute even against the longer illustrative
 * durations (see kf_pet_default_config()). */
constexpr DebugButton kButtons[] = {
    {{16, 16, 100, 32}, "Skip 1 Hour", DebugAction::kSkipHour},
    {{124, 16, 100, 32}, "Skip 1 Day", DebugAction::kSkipDay},
    {{232, 16, 92, 32}, "Skip 1 Week", DebugAction::kSkipWeek},

    {{16, 56, 100, 32}, "Reset Egg", DebugAction::kReset},
    {{124, 56, 100, 32}, "Save Now", DebugAction::kSave},
    /* Fills the row's remaining width rather than starting a new row --
     * see kf_screen_nav.h for what this actually calls and why it exists
     * as a separate entry point from the keyboard's MENU binding. */
    {{232, 56, 152, 32}, "Next Screen", DebugAction::kNextScreen},

    {{16, 96, 64, 32}, "1x", DebugAction::kMult1},
    {{88, 96, 64, 32}, "2x", DebugAction::kMult2},
    {{160, 96, 64, 32}, "4x", DebugAction::kMult4},
    {{232, 96, 64, 32}, "8x", DebugAction::kMult8},
    {{304, 96, 64, 32}, "16x", DebugAction::kMult16},

    {{16, 136, 64, 32}, "32x", DebugAction::kMult32},
    {{88, 136, 64, 32}, "64x", DebugAction::kMult64},
    {{160, 136, 64, 32}, "128x", DebugAction::kMult128},
    {{232, 136, 64, 32}, "256x", DebugAction::kMult256},

    /* Task 8: picks which teen_form/adult_branch the stage-jump row below
     * will use -- read this row (and the next) top-to-bottom BEFORE the
     * jump row, the same order a player presses them in. Four teen_form
     * slots (KF_PET_TEEN_FORM_COUNT) plus dust (see kTeenFormDust's own
     * comment above) and three adult_branch slots (KF_PET_ADULT_BRANCH_MAX,
     * the widest family) -- see kf/pet.h. An adult_branch picked here that
     * is out of range for whichever teen_form is ALSO currently selected is
     * not rejected here (this window does not know each family's exact
     * count without calling kf_pet_adults_in_family(), which it happily
     * could, but the session layer already clamps it defensively -- see
     * kf_pet_session_debug_jump_to_stage()'s own header comment), so
     * pressing e.g. "Adult 2" against a one-adult family is harmless, just
     * not useful. That same clamp is also exactly what makes "Dust" safe to
     * combine with any Adult button: kf_pet_adults_in_family() returns 1
     * for dust by construction (kf/pet.h), so anything but Adult0 there
     * just falls back to it. */
    {{16, 176, 64, 32}, "Form0", DebugAction::kTeenForm0},
    {{88, 176, 64, 32}, "Form1", DebugAction::kTeenForm1},
    {{160, 176, 64, 32}, "Form2", DebugAction::kTeenForm2},
    {{232, 176, 64, 32}, "Form3", DebugAction::kTeenForm3},
    {{304, 176, 64, 32}, "Dust", DebugAction::kTeenFormDust},

    {{16, 216, 64, 32}, "Adult0", DebugAction::kAdultBranch0},
    {{88, 216, 64, 32}, "Adult1", DebugAction::kAdultBranch1},
    {{160, 216, 64, 32}, "Adult2", DebugAction::kAdultBranch2},

    /* Task 8: the actual stage jump -- "spawns the creature at full care
     * stats... at each button press and timeline moves to the beginning
     * of each life stage" (Chris's own words). Uses whichever teen_form/
     * adult_branch the two rows above currently have selected (0/0 unless
     * changed); see kf_pet_session_debug_jump_to_stage() for what "start
     * of a stage" guarantees. */
    {{16, 256, 64, 32}, "Egg", DebugAction::kJumpEgg},
    {{88, 256, 64, 32}, "Baby", DebugAction::kJumpBaby},
    {{160, 256, 64, 32}, "Child", DebugAction::kJumpChild},
    {{232, 256, 64, 32}, "Teen", DebugAction::kJumpTeen},
    {{304, 256, 64, 32}, "Adult", DebugAction::kJumpAdult},

    /* Task 8: "I want a button to just autoprogress" -- one press, one
     * stage forward, from wherever the pet currently is. See perform()'s
     * kJumpNextStage case for exactly what "forward" means once already
     * at Adult. */
    {{16, 296, 150, 32}, "Next Stage", DebugAction::kJumpNextStage},

    /* The sleep-cycle row (2026-08-11, Chris: "add a button to the debug
     * window to take the clock to bedtime ... I want to see if the clock
     * time forces the pet to bed"). Three points rather than the one that
     * was asked for, because the cycle has three moments worth seeing and
     * the other two were free once the clock-setting plumbing existed:
     * Drowsy is when tuck-in becomes available and is therefore the ONLY
     * way to see the futon without waiting for 21:50 to come round;
     * Morning is the other end of the same window. See the enum above for
     * why two of the three land ten seconds early. */
    {{16, 336, 84, 32}, "Drowsy", DebugAction::kClockDrowsy},
    {{108, 336, 84, 32}, "Bedtime", DebugAction::kClockBedtime},
    {{200, 336, 84, 32}, "Morning", DebugAction::kClockMorning},
    {{292, 336, 92, 32}, "Sync Clock", DebugAction::kClockSyncHost},
};

/* Duplicated from sdl_main.cpp's identical helper and kf_pet_screen.cpp's
 * blob caption switch -- three copies of four lines of string mapping is
 * cheaper to keep in sync by inspection than a shared header would be to
 * maintain, the same call this codebase already made once (see
 * sdl_main.cpp's own comment on its copy). Also used for the timeline's
 * per-stage tick labels below. */
const char *stage_name(kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG:
        return "egg";
    case KF_PET_STAGE_BABY:
        return "baby";
    case KF_PET_STAGE_CHILD:
        return "child";
    case KF_PET_STAGE_TEEN:
        return "teen";
    case KF_PET_STAGE_ADULT:
    default:
        return "adult";
    }
}

/* Stage duration, read from kf_pet_default_config() -- not from
 * kf_pet_session's own live config, which is not exposed outside
 * kf_pet_session.cpp (see kf_pet_session.h: only the live STATE is
 * readable, deliberately, since nothing today ever overrides the
 * config a session boots with). Accurate today because kf_pet_session_
 * init() always uses exactly this default; would need a real getter if
 * that ever stops being true. */
uint32_t stage_duration_seconds(const kf_pet_config &config,
                                 kf_pet_stage stage) {
    switch (stage) {
    case KF_PET_STAGE_EGG:
        return config.egg_duration_seconds;
    case KF_PET_STAGE_BABY:
        return config.baby_duration_seconds;
    case KF_PET_STAGE_CHILD:
        return config.child_duration_seconds;
    case KF_PET_STAGE_TEEN:
        return config.teen_duration_seconds;
    case KF_PET_STAGE_ADULT:
    default:
        return 0u; /* terminal -- no duration to compare against */
    }
}

/* The timeline's X axis: total pet-age (kf_pet_session_debug_age_seconds()'s
 * own convention -- cumulative stage durations, NOT wall-clock or session
 * uptime) at which each stage BEGINS. Same duplication call as stage_
 * duration_seconds() above, and the same sum kf_pet_session.cpp's own
 * (private) elapsed_before_stage() computes -- this file has no way to
 * reach that one, and four lines of addition is not worth exporting it
 * for. */
uint64_t timeline_tick_seconds(const kf_pet_config &config, kf_pet_stage stage) {
    uint64_t t = 0u;
    if (stage > KF_PET_STAGE_EGG) {
        t += config.egg_duration_seconds;
    }
    if (stage > KF_PET_STAGE_BABY) {
        t += config.baby_duration_seconds;
    }
    if (stage > KF_PET_STAGE_CHILD) {
        t += config.child_duration_seconds;
    }
    if (stage > KF_PET_STAGE_TEEN) {
        t += config.teen_duration_seconds;
    }
    return t;
}

/* Where the timeline ends: the moment Adult begins. Adult itself is
 * terminal (no duration, see kf/pet.h) so there is no further scheduled
 * mark to draw past this point -- see the timeline's own draw/seek code
 * below for how a pet already in Adult is handled (marker pinned at the
 * right edge; scrubbing stays bounded to [0, this]). */
uint64_t timeline_axis_max_seconds(const kf_pet_config &config) {
    return timeline_tick_seconds(config, KF_PET_STAGE_ADULT);
}

/* y moved from 200 to make room for the four Task 8 button rows above it
 * (teen_form picker, adult_branch picker, the stage-jump row, Next Stage --
 * see kButtons and kWindowH's own comments above), then 344 -> 384 again
 * for the sleep-cycle row. */
constexpr SDL_FRect kTimelineBar = {16, 384, kLeftColumnW - 32, 14};
/* Grabbable beyond the bar's own drawn height -- a 14px-tall target is
 * fiddly to click precisely; this widens the hit region without widening
 * what's actually drawn. Vertical position only matters for STARTING a
 * drag (see kf_sdl_debug_window_frame() below); once dragging, only the
 * X coordinate matters, same as any scrub bar. */
constexpr SDL_FRect kTimelineHitRect = {kTimelineBar.x, kTimelineBar.y - 10,
                                         kTimelineBar.w, kTimelineBar.h + 20};

struct Session {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool previous_pressed = false;
    bool timeline_dragging = false;
    uint32_t multiplier = 1;

    /* Task 8: which teen_form/adult_branch the stage-jump buttons and Next
     * Stage will pass to kf_pet_session_debug_jump_to_stage() -- "defaults
     * to the first one if not set" (Chris's own words), which is exactly
     * what 0 already means for both indices, so no separate "unset" state
     * is needed here at all. Changed only by the Form-N/Adult-N buttons
     * below; a stage jump never changes these itself, so the same pick
     * survives across repeated stage jumps until the player deliberately
     * changes it. */
    uint8_t selected_teen_form = 0;
    uint8_t selected_adult_branch = 0;
};
Session g;

uint32_t multiplier_for(DebugAction action) {
    switch (action) {
    case DebugAction::kMult1:
        return 1u;
    case DebugAction::kMult2:
        return 2u;
    case DebugAction::kMult4:
        return 4u;
    case DebugAction::kMult8:
        return 8u;
    case DebugAction::kMult16:
        return 16u;
    case DebugAction::kMult32:
        return 32u;
    case DebugAction::kMult64:
        return 64u;
    case DebugAction::kMult128:
        return 128u;
    case DebugAction::kMult256:
        return 256u;
    default:
        return 0u;
    }
}

bool is_multiplier_button(DebugAction action) { return multiplier_for(action) != 0u; }

/* Task 8: the same "map the enum value to what it means, a default of
 * false/0 for anything else" shape as multiplier_for()/is_multiplier_
 * button() just above, repeated for the two new pickers rather than
 * generalised into one templated helper -- three tiny, obviously-correct
 * functions read easier at a glance than one that has to be parameterised
 * over which enum range and which Session field it is working with. */
bool is_teen_form_button(DebugAction action) {
    return action == DebugAction::kTeenForm0 ||
           action == DebugAction::kTeenForm1 ||
           action == DebugAction::kTeenForm2 ||
           action == DebugAction::kTeenForm3 ||
           action == DebugAction::kTeenFormDust;
}

uint8_t teen_form_for(DebugAction action) {
    switch (action) {
    case DebugAction::kTeenForm0:
        return 0u;
    case DebugAction::kTeenForm1:
        return 1u;
    case DebugAction::kTeenForm2:
        return 2u;
    case DebugAction::kTeenForm3:
        return 3u;
    case DebugAction::kTeenFormDust:
        return KF_PET_TEEN_FORM_DUST;
    default:
        return 0u;
    }
}

bool is_adult_branch_button(DebugAction action) {
    return action == DebugAction::kAdultBranch0 ||
           action == DebugAction::kAdultBranch1 ||
           action == DebugAction::kAdultBranch2;
}

uint8_t adult_branch_for(DebugAction action) {
    switch (action) {
    case DebugAction::kAdultBranch0:
        return 0u;
    case DebugAction::kAdultBranch1:
        return 1u;
    case DebugAction::kAdultBranch2:
        return 2u;
    default:
        return 0u;
    }
}

/* Task 8: which kf_pet_stage a jump button targets -- kJumpNextStage is
 * deliberately excluded (its target depends on the pet's CURRENT stage,
 * computed in perform() below, not a fixed value a lookup table can hold),
 * same reason it is not a multiplier/form/branch-style "pick a value"
 * button. */
bool is_jump_stage_button(DebugAction action) {
    return action == DebugAction::kJumpEgg || action == DebugAction::kJumpBaby ||
           action == DebugAction::kJumpChild ||
           action == DebugAction::kJumpTeen || action == DebugAction::kJumpAdult;
}

kf_pet_stage jump_stage_for(DebugAction action) {
    switch (action) {
    case DebugAction::kJumpEgg:
        return KF_PET_STAGE_EGG;
    case DebugAction::kJumpBaby:
        return KF_PET_STAGE_BABY;
    case DebugAction::kJumpChild:
        return KF_PET_STAGE_CHILD;
    case DebugAction::kJumpTeen:
        return KF_PET_STAGE_TEEN;
    case DebugAction::kJumpAdult:
    default:
        return KF_PET_STAGE_ADULT;
    }
}

/* Every button below that maps onto a portable debug action goes through
 * kf_debug_actions.h's table rather than calling the session directly (ADR
 * 0060). The button LAYOUT stays here -- rectangles, labels, which fixed
 * argument each button supplies -- because that is presentation and the
 * serial bridge has no use for it. What moved is the BEHAVIOUR, so that this
 * window and KFDBG cannot offer different sets of debug actions.
 *
 * Aborts loudly on an unknown verb rather than silently doing nothing: a
 * typo'd verb here is a dead button, which is exactly the class of quiet
 * failure this whole mechanism exists to prevent. The parity checker
 * (tools/check_debug_parity.py) catches it before a build, but this catches
 * it if someone bypasses the checker. */
void run_shared(const char *verb, const kf_debug_args &args) {
    const kf_debug_action *action = kf_debug_action_find(verb);
    if (action == nullptr) {
        KF_LOGE("dbgwin", "debug button names verb '%s', which is not in "
                          "kf_debug_actions.cpp's table -- button is dead",
                verb);
        return;
    }
    action->run(&args);
}

void run_shared(const char *verb) {
    const kf_debug_args args{};
    run_shared(verb, args);
}

void run_shared_u32(const char *verb, uint32_t value) {
    kf_debug_args args{};
    args.value = value;
    run_shared(verb, args);
}

void perform(DebugAction action) {
    switch (action) {
    /* All three go through kf_pet_session_debug_clock_target() rather than
     * naming times here, so the headless clock_jump_check asserts against
     * the times these buttons ACTUALLY jump to. The first version of this
     * row did name them here, aimed ten seconds SHORT of each transition,
     * and appeared to do nothing -- see that function's header comment. */
    case DebugAction::kClockDrowsy: {
        kf_debug_args args{};
        args.clock = KF_DEBUG_CLOCK_DROWSY;
        run_shared("CLOCK", args);
        break;
    }
    case DebugAction::kClockBedtime: {
        kf_debug_args args{};
        args.clock = KF_DEBUG_CLOCK_BEDTIME;
        run_shared("CLOCK", args);
        break;
    }
    case DebugAction::kClockMorning: {
        kf_debug_args args{};
        args.clock = KF_DEBUG_CLOCK_MORNING;
        run_shared("CLOCK", args);
        break;
    }
    case DebugAction::kClockSyncHost: {
        /* kf_host_time_system_now(), NOT kf_time_wall() -- the latter
         * reports the simulated clock this button exists to correct, so
         * using it would make the button a no-op that looks like it worked.
         *
         * Resolved to an absolute epoch HERE and handed to the shared CLOCK
         * action as one, rather than being a fifth clock target the table
         * knows how to compute: kf_host_time_system_now() is desktop-only
         * and is not part of the device build at all. The device reaches the
         * identical code path with KFDBG CLOCK EPOCH <n>, which is what
         * tools/kf_debug.py's `clock sync` sends after reading the host
         * clock on its own side of the wire. */
        kf_debug_args args{};
        args.clock = KF_DEBUG_CLOCK_EPOCH;
        args.epoch_seconds = kf_host_time_system_now();
        run_shared("CLOCK", args);
        break;
    }
    case DebugAction::kSkipHour:
        run_shared_u32("ADVANCE", 3600u);
        break;
    case DebugAction::kSkipDay:
        run_shared_u32("ADVANCE", 24u * 3600u);
        break;
    case DebugAction::kSkipWeek:
        run_shared_u32("ADVANCE", 7u * 24u * 3600u);
        break;
    case DebugAction::kReset:
        run_shared("RESET");
        break;
    case DebugAction::kSave:
        run_shared("SAVE");
        break;
    case DebugAction::kNextScreen:
        /* Same effect a real MENU press has on which pet-window screen is
         * loaded -- see kf_screen_nav.h -- but this path means
         * kf_app_frame() never sees a KF_BTN_MENU edge, so Core's on-device
         * HUD toggle (kf/app.cpp) never fires either. That is the entire
         * point of this button; see this file's own header comment. The
         * device gained the same thing as KFDBG SCREEN with ADR 0060 --
         * it previously had only BTN, which fires the real edge. */
        run_shared("SCREEN");
        break;
    case DebugAction::kJumpNextStage: {
        /* "I want a button to just autoprogress" -- one press, one stage
         * forward from wherever the pet currently is. The reasoning about
         * Adult being terminal now lives with the behaviour, in
         * kf_debug_actions.cpp's run_next_stage(); the device gained the
         * same thing as KFDBG NEXTSTAGE with ADR 0060, having previously
         * had only the absolute JUMP. */
        kf_debug_args args{};
        args.teen_form = g.selected_teen_form;
        args.adult_branch = g.selected_adult_branch;
        run_shared("NEXTSTAGE", args);
        break;
    }
    default:
        /* The three picker families below set THIS WINDOW'S selection state
         * and perform no pet action, which is why they are not table verbs:
         * on the serial side the equivalent is passing teen_form/
         * adult_branch as arguments to JUMP directly, and the multiplier is
         * per-backend by design (see kf_debug_actions.h). */
        if (is_multiplier_button(action)) {
            g.multiplier = multiplier_for(action);
        } else if (is_teen_form_button(action)) {
            g.selected_teen_form = teen_form_for(action);
        } else if (is_adult_branch_button(action)) {
            g.selected_adult_branch = adult_branch_for(action);
        } else if (is_jump_stage_button(action)) {
            kf_debug_args args{};
            args.value = static_cast<uint32_t>(jump_stage_for(action));
            args.teen_form = g.selected_teen_form;
            args.adult_branch = g.selected_adult_branch;
            run_shared("JUMP", args);
        }
        break;
    }
}

bool point_in_rect(int32_t x, int32_t y, const SDL_FRect &rect) {
    return static_cast<float>(x) >= rect.x &&
           static_cast<float>(x) < rect.x + rect.w &&
           static_cast<float>(y) >= rect.y &&
           static_cast<float>(y) < rect.y + rect.h;
}

void draw_button(const DebugButton &b) {
    /* Highlighted whenever the button represents the CURRENTLY selected
     * value of whichever pick it belongs to -- the multiplier, same as
     * before Task 8, plus the two new Task 8 pickers (teen_form/
     * adult_branch). Stage-jump and Next Stage buttons are one-shot
     * actions, not picks, so neither is ever "active" in this sense --
     * is_teen_form_button()/is_adult_branch_button() are false for them,
     * same as is_multiplier_button() already is. */
    const bool active =
        (is_multiplier_button(b.action) &&
         multiplier_for(b.action) == g.multiplier) ||
        (is_teen_form_button(b.action) &&
         teen_form_for(b.action) == g.selected_teen_form) ||
        (is_adult_branch_button(b.action) &&
         adult_branch_for(b.action) == g.selected_adult_branch);
    if (active) {
        SDL_SetRenderDrawColor(g.renderer, 40, 120, 220, 255);
    } else {
        SDL_SetRenderDrawColor(g.renderer, 60, 60, 66, 255);
    }
    SDL_RenderFillRect(g.renderer, &b.rect);
    SDL_SetRenderDrawColor(g.renderer, 200, 200, 210, 255);
    SDL_RenderRect(g.renderer, &b.rect);
    SDL_RenderDebugText(g.renderer, b.rect.x + 6, b.rect.y + 12, b.label);
}

/* Maps a seconds-on-the-axis value to an X pixel on the timeline bar,
 * clamped to the bar itself -- used for both the marker (current age)
 * and the tick marks (stage-start times). A pet in Adult has an age
 * past axis_max with nothing further to show, so it draws pinned at the
 * right edge rather than off the end of the bar. */
float timeline_x_for(uint64_t seconds, uint64_t axis_max) {
    if (axis_max == 0u) {
        return kTimelineBar.x;
    }
    const double frac =
        static_cast<double>(seconds < axis_max ? seconds : axis_max) /
        static_cast<double>(axis_max);
    return kTimelineBar.x + static_cast<float>(frac) * kTimelineBar.w;
}

/* Inverse of timeline_x_for() -- a pixel X on (or dragged past) the bar
 * back to a seconds value, for scrubbing. Clamped to [0, axis_max]: you
 * cannot drag past the last scheduled mark (Adult, terminal) any more
 * than the bar itself draws past it. */
uint64_t timeline_seconds_for_x(int32_t x, uint64_t axis_max) {
    const float x_f = static_cast<float>(x);
    const float clamped_x =
        x_f < kTimelineBar.x ? kTimelineBar.x
        : x_f > kTimelineBar.x + kTimelineBar.w ? kTimelineBar.x + kTimelineBar.w
                                                 : x_f;
    const float frac =
        kTimelineBar.w > 0.0f ? (clamped_x - kTimelineBar.x) / kTimelineBar.w : 0.0f;
    return static_cast<uint64_t>(static_cast<double>(frac) *
                                  static_cast<double>(axis_max));
}

void draw_timeline(const kf_pet_config &config, kf_pet_stage current_stage,
                    uint64_t current_age) {
    const uint64_t axis_max = timeline_axis_max_seconds(config);

    SDL_SetRenderDrawColor(g.renderer, 50, 50, 56, 255);
    SDL_RenderFillRect(g.renderer, &kTimelineBar);
    SDL_SetRenderDrawColor(g.renderer, 200, 200, 210, 255);
    SDL_RenderRect(g.renderer, &kTimelineBar);

    /* One tick per stage START, egg through adult -- exactly the moments
     * Chris asked to see coming: "marks for when each evolution/life
     * stage happens." */
    constexpr kf_pet_stage kStages[] = {KF_PET_STAGE_EGG, KF_PET_STAGE_BABY,
                                         KF_PET_STAGE_CHILD, KF_PET_STAGE_TEEN,
                                         KF_PET_STAGE_ADULT};
    SDL_SetRenderDrawColor(g.renderer, 150, 150, 158, 255);
    for (size_t i = 0; i < 5; ++i) {
        const kf_pet_stage stage = kStages[i];
        const float x = timeline_x_for(timeline_tick_seconds(config, stage), axis_max);
        /* Egg is a sliver of the full axis (1 hour out of ~6 days by
         * default) -- its tick sits only a couple of pixels from baby's,
         * far closer together than either label is wide. Alternating
         * label rows (even stages on one line, odd on the line below)
         * keeps adjacent labels legible regardless of how lopsided the
         * configured stage durations are, rather than only patching the
         * egg/baby case specifically. */
        const float label_y = kTimelineBar.y + kTimelineBar.h + 8 +
                               (i % 2u == 0u ? 0.0f : 10.0f);
        SDL_RenderLine(g.renderer, x, kTimelineBar.y - 4, x, label_y);
        SDL_RenderDebugText(g.renderer, x - 12, label_y + 4, stage_name(stage));
    }

    /* The marker: current age, pinned at the right edge once Adult is
     * reached (see timeline_x_for()). Drawn last so it sits on top of
     * the tick marks it may currently coincide with. */
    const float marker_x = timeline_x_for(current_age, axis_max);
    const SDL_FRect marker = {marker_x - 2, kTimelineBar.y - 6, 4,
                               kTimelineBar.h + 12};
    SDL_SetRenderDrawColor(g.renderer, 255, 140, 40, 255);
    SDL_RenderFillRect(g.renderer, &marker);

    (void)current_stage;
}

/* Mirrors what Core's on-device constraint HUD (kf/app.cpp's draw_hud(),
 * ADR 0010) would show, in this window's own right-hand column instead of
 * drawn over the pet screen -- see this file's own header comment for
 * why. Reads the exact same public accessors draw_hud() itself reads
 * (kf_app_last_frame(), kf_app_frame_summary(), kf_arena_get_stats()),
 * not a shadow copy of Core's own accounting -- if Core's numbers change,
 * so do these, automatically, the same as the real HUD would. A friendlier
 * multi-line layout than the real HUD's compact 40-column format: that
 * format exists because the real HUD has to fit a font budget and a
 * dirty-rect cost on the actual device (see ADR 0010's "Why the HUD
 * defaults off"), neither of which applies to a plain SDL window that
 * clears and redraws itself unconditionally every frame regardless of
 * position. */
void draw_engine_diagnostics(void) {
    SDL_SetRenderDrawColor(g.renderer, 70, 70, 78, 255);
    SDL_RenderLine(g.renderer, static_cast<float>(kLeftColumnW),
                    0.0f, static_cast<float>(kLeftColumnW),
                    static_cast<float>(kWindowH));

    char line[128];
    float y = 16.0f;
    constexpr float kLineHeight = 18.0f;
    SDL_SetRenderDrawColor(g.renderer, 220, 220, 225, 255);

    SDL_RenderDebugText(g.renderer, kRightColumnX, y, "-- engine (last frame) --");
    y += kLineHeight * 1.5f;

    const kf_frame_stats *last = kf_app_last_frame();
    const uint32_t fps_tenths =
        last->total_us > 0u
            ? static_cast<uint32_t>(10000000ull / last->total_us)
            : 0u;
    std::snprintf(line, sizeof(line), "fps: %u.%u", fps_tenths / 10u,
                  fps_tenths % 10u);
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "frame: %u us", last->total_us);
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "dirty: %u%%  rects: %u",
                  static_cast<unsigned>(last->dirty_percent),
                  static_cast<unsigned>(last->dirty_rect_count));
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight;

    /* Always draws SOMETHING on this line, in-budget or not -- unlike
     * Core's own draw_hud() (see its pad_to() comment), this window has
     * no leftover-glyph problem at all: SDL_RenderClear() below wipes the
     * whole window every frame, so there is nothing to accidentally leave
     * behind by having a shorter string than last frame. This just keeps
     * the rest of the column's y-position stable frame to frame either
     * way. */
    SDL_RenderDebugText(g.renderer, kRightColumnX, y,
                         last->over_budget ? "OVER BUDGET" : "within budget");
    y += kLineHeight * 1.5f;

    const kf_frame_summary summary = kf_app_frame_summary();
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, "-- frame summary --");
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "mean: %u us  p99: %u us",
                  summary.mean_us, summary.p99_us);
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "worst: %u us", summary.worst_us);
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "frames: %llu  over: %llu",
                  static_cast<unsigned long long>(summary.frames),
                  static_cast<unsigned long long>(summary.over_budget_frames));
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
    y += kLineHeight * 1.5f;

    /* Every arena this build actually carves -- four by default, matching
     * the on-device HUD's own four (kf/app.cpp's draw_hud()); a fifth
     * (lvgl) appears only when built with -DKF_ENABLE_LVGL=ON (ADR 0045 --
     * KF_ARENA_LVGL itself does not exist in kf/arena.h's enum otherwise).
     * Nothing about this column is squeezed for space the way the real HUD
     * is, so there is no reason to leave one out when it exists. */
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, "-- arenas (hi/cap KB) --");
    y += kLineHeight;

#ifdef KF_ENABLE_LVGL
    constexpr kf_arena_id kArenas[] = {KF_ARENA_FRAMEBUFFER, KF_ARENA_SCRATCH,
                                       KF_ARENA_LUA, KF_ARENA_ASSETS,
                                       KF_ARENA_LVGL};
#else
    constexpr kf_arena_id kArenas[] = {KF_ARENA_FRAMEBUFFER, KF_ARENA_SCRATCH,
                                       KF_ARENA_LUA, KF_ARENA_ASSETS};
#endif
    for (kf_arena_id arena : kArenas) {
        const kf_arena_stats *s = kf_arena_get_stats(arena);
        std::snprintf(line, sizeof(line), "%-11s %u/%uK", s->name,
                      static_cast<unsigned>(s->high_water_bytes / 1024u),
                      static_cast<unsigned>(s->capacity_bytes / 1024u));
        SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
        y += kLineHeight;
    }
}

/* Added after Chris's pet died and this window could not say why -- the
 * whole reason for this file's death-diagnostics block below. What Core
 * actually compares `state->neglect_seconds` against is NOT
 * config.sickness_onset_seconds/sickness_death_seconds directly: nights
 * (22:00-07:00) do not accrue neglect, so once a wall clock exists,
 * apply_stage_segment() (hakoniwaos/src/pet.cpp) reads each threshold
 * through a 15/24 "waking fraction" compression first -- a day's worth of
 * neglect-worthy time is only about 15 of its 24 hours, so the thresholds
 * are shrunk to match, and a readout that skipped this step would show a
 * death threshold roughly 60% too generous. This is NOT a life-stage
 * scaling -- kf_pet_config has no per-stage table for either threshold,
 * only stage_rates (the three needs' decay rates) does -- so this window
 * does not attempt to show "the value for the current stage" the way the
 * timeline's stage_duration_seconds() above does; there is nothing
 * stage-shaped to show here, only this one clock-dependent compression.
 *
 * `have_clock` mirrors apply_stage_segment()'s own local of the same name
 * exactly: `state->last_advanced.valid` at the moment a segment is
 * evaluated. With no wall clock ever established there is no day/night
 * overlap to compute at all, so Core falls back to the raw, uncompressed
 * config values, and so does this.
 *
 * The 15/24 duplicated here rather than exported from kf/pet.h or
 * hakoniwaos/src/pet.cpp -- kWakingFractionNumerator/Denominator there are
 * anonymous-namespace locals, not part of Core's public surface, and four
 * lines of integer arithmetic is not worth widening that surface for. Same
 * "cheaper to keep in sync by inspection" call this file already made three
 * times over for stage_name()/stage_duration_seconds()/
 * timeline_tick_seconds() above: if pet.cpp's compression ever changes,
 * this has to change by hand, with no compiler to catch the drift. */
uint32_t effective_sickness_seconds(uint32_t raw_seconds, bool have_clock) {
    constexpr uint32_t kWakingFractionNumerator = 15u;
    constexpr uint32_t kWakingFractionDenominator = 24u;
    if (!have_clock) {
        return raw_seconds;
    }
    return static_cast<uint32_t>((static_cast<uint64_t>(raw_seconds) *
                                   kWakingFractionNumerator) /
                                  kWakingFractionDenominator);
}

/* Formats a duration as "%uh%02um" -- e.g. "4h12m", "15h00m" -- for the
 * neglect ratio and time-to-death lines below. A bare second count
 * (`10800s`) is exactly the "tells the owner nothing about how much
 * runway is left" complaint that prompted this whole block: nobody reads
 * seconds-since-boot as a felt duration, and the mental division into
 * hours/minutes is the entire missing piece. Not shared with the h/m-free
 * formatting the rest of this window already uses elsewhere (age, stage
 * time) -- those are deliberately raw seconds against a raw axis for the
 * timeline's own scrubbing math, a different job from "how worried should
 * I be right now". */
void format_hm(uint32_t total_seconds, char *out, size_t out_size) {
    const uint32_t hours = total_seconds / 3600u;
    const uint32_t minutes = (total_seconds % 3600u) / 60u;
    std::snprintf(out, out_size, "%uh%02um", static_cast<unsigned>(hours),
                  static_cast<unsigned>(minutes));
}

/* Which of is_neglected()'s five conditions (hakoniwaos/src/pet.cpp) is
 * ACTUALLY responsible for the creature's neglect clock climbing right
 * now, written out in words -- "neglected" on its own leaves the owner
 * guessing which of five things to go fix. is_neglected() itself is
 * file-local to Core (an anonymous-namespace helper, not declared in
 * kf/pet.h) and this window has no way to call it, so its predicate is
 * re-derived here from the same public state/config fields it reads --
 * again the stage_name()-style duplication call, not a new Core accessor,
 * for the same "four lines, not worth exporting" reasoning as the function
 * above.
 *
 * Reports EVERY condition currently past its line, not only the first: a
 * creature that is both starving and filthy is two separate mistakes, and
 * naming just one would send the owner to fix the wrong thing first and
 * report back confused when the pet was still dying afterwards. */
void describe_neglect_driver(char *out, size_t out_size,
                              const kf_pet_state *state,
                              const kf_pet_config *config) {
    const char *conditions[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t count = 0u;
    if (state->hunger_mp <= config->neglect_need_mp) {
        conditions[count++] = "hunger";
    }
    if (state->happiness_mp <= config->neglect_need_mp) {
        conditions[count++] = "happiness";
    }
    if (state->energy_mp <= config->neglect_need_mp) {
        conditions[count++] = "energy";
    }
    if (state->poop_count > config->neglect_poop_count) {
        conditions[count++] = "poop count";
    }
    if (state->dirtiness_mp >= config->neglect_dirtiness_mp) {
        conditions[count++] = "dirtiness";
    }

    if (count == 0u) {
        std::snprintf(out, out_size, "none (currently cared for)");
        return;
    }
    size_t offset = 0u;
    for (size_t i = 0u; i < count && offset < out_size; ++i) {
        const int written =
            std::snprintf(out + offset, out_size - offset, "%s%s",
                           i == 0u ? "" : ", ", conditions[i]);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        }
    }
}

/* The other half of the death chain: not what is happening NOW
 * (describe_neglect_driver(), above) but what killed the pet LAST time, if
 * it ever has -- kf_pet_session_last_death_cause() (kf_pet_session.h),
 * read from its own store key rather than kf_pet_state so a corrupt or
 * missing record can never take a save down with it. Same "list every
 * condition, not just the first" shape as describe_neglect_driver() above,
 * and the same reason: a creature that died with three separate needs
 * empty at once is not usefully summarised by naming only one of them. */
void describe_death_cause(char *out, size_t out_size,
                           const kf_pet_death_cause *cause) {
    if (!cause->known) {
        std::snprintf(out, out_size, "unknown (no record yet)");
        return;
    }
    const char *conditions[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t count = 0u;
    if (cause->hunger) {
        conditions[count++] = "hunger";
    }
    if (cause->happiness) {
        conditions[count++] = "happiness";
    }
    if (cause->energy) {
        conditions[count++] = "energy";
    }
    if (cause->poop_count) {
        conditions[count++] = "poop count";
    }
    if (cause->dirtiness) {
        conditions[count++] = "dirtiness";
    }
    if (count == 0u) {
        /* Reachable only if a record exists but every bit is clear -- not
         * expected from record_death_cause() (kf_pet_session.cpp), which
         * only ever writes a fresh record on a genuine `dead` edge and
         * is_neglected()'s five-way OR guarantees at least one condition
         * was true for that edge to have fired at all, but shown as
         * something other than a silent blank line if it ever happens,
         * rather than asserting a desktop diagnostics window down over a
         * display-only inconsistency. */
        std::snprintf(out, out_size, "recorded, but no condition flagged");
        return;
    }
    size_t offset = 0u;
    for (size_t i = 0u; i < count && offset < out_size; ++i) {
        const int written =
            std::snprintf(out + offset, out_size - offset, "%s%s",
                           i == 0u ? "" : ", ", conditions[i]);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        }
    }
}

} // namespace

void kf_sdl_debug_window_init(void) {
    if (!SDL_CreateWindowAndRenderer("Kamiframe debug", kWindowW, kWindowH, 0,
                                      &g.window, &g.renderer)) {
        KF_LOGE(TAG, "SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return;
    }

    /* Position next to the pet window rather than wherever the window
     * manager happens to default to -- the whole point is having both
     * visible together. Best-effort: if the pet window's own position
     * cannot be read yet for some reason, this just leaves the debug
     * window at the window manager's default spot instead. */
    KfSdlState &s = kf_sdl_state();
    if (s.window != nullptr) {
        int px = 0;
        int py = 0;
        int pw = 0;
        int ph = 0;
        SDL_GetWindowPosition(s.window, &px, &py);
        SDL_GetWindowSize(s.window, &pw, &ph);
        SDL_SetWindowPosition(g.window, px + pw + 12, py);
    }

    s.debug_window = g.window;
    s.debug_window_close_requested = false;
    g.previous_pressed = false;
    g.timeline_dragging = false;
    g.multiplier = 1u;

    KF_LOGI(TAG, "debug window ready -- controls are on that window now, "
                 "not this terminal or the pet screen");
}

void kf_sdl_debug_window_frame(void) {
    KfSdlState &s = kf_sdl_state();

    if (s.debug_window_close_requested) {
        SDL_DestroyRenderer(g.renderer);
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
        g.renderer = nullptr;
        s.debug_window = nullptr;
        s.debug_window_close_requested = false;
        KF_LOGI(TAG, "debug window closed (the pet window is unaffected)");
        return;
    }

    if (g.window == nullptr) {
        return;
    }

    const kf_pet_config config = kf_pet_default_config();
    const uint64_t axis_max = timeline_axis_max_seconds(config);

    int32_t mouse_x = 0;
    int32_t mouse_y = 0;
    bool pressed = false;
    kf_sdl_mouse_relative_to(g.window, &mouse_x, &mouse_y, &pressed);
    const bool clicked_now = pressed && !g.previous_pressed;

    if (!pressed) {
        /* Released (or never was pressed over this window this frame) --
         * a drag, if any, ends here. */
        g.timeline_dragging = false;
    }

    if (clicked_now && point_in_rect(mouse_x, mouse_y, kTimelineHitRect)) {
        g.timeline_dragging = true;
    }

    if (g.timeline_dragging && pressed) {
        /* Every frame the drag is held, not just on the initial click --
         * this is what makes it a scrub, not a click-to-set-point. Only
         * the X coordinate matters once a drag has started (see
         * kTimelineHitRect's own comment on why). */
        kf_pet_session_debug_seek(timeline_seconds_for_x(mouse_x, axis_max));
    } else if (clicked_now) {
        for (const DebugButton &b : kButtons) {
            if (point_in_rect(mouse_x, mouse_y, b.rect)) {
                perform(b.action);
                break;
            }
        }
    }
    g.previous_pressed = pressed;

    SDL_SetRenderDrawColor(g.renderer, 24, 24, 28, 255);
    SDL_RenderClear(g.renderer);

    for (const DebugButton &b : kButtons) {
        draw_button(b);
    }

    /* Live readout -- everything a person testing time-based mechanics
     * would otherwise have to squint at bars or wait real hours to see. */
    const kf_pet_state *state = kf_pet_session_state();
    const uint32_t duration = stage_duration_seconds(config, state->stage);
    const uint64_t current_age = kf_pet_session_debug_age_seconds();

    draw_timeline(config, state->stage, current_age);

    char line[128];
    /* Moved from 250 to make room for the four Task 8 button rows above
     * the timeline -- see kWindowH's and kTimelineBar's own comments. */
    /* 400 -> 440 with the sleep-cycle button row; moves in lockstep with
     * kTimelineBar and kWindowH -- see kWindowH's own comment. */
    float y = 440.0f;
    constexpr float kLineHeight = 18.0f;
    SDL_SetRenderDrawColor(g.renderer, 220, 220, 225, 255);

    /* The wall clock, and how far it has drifted from the host's.
     *
     * Added 2026-08-12 because Chris reported "the sync clock button
     * appears to be doing nothing" -- and it wasn't broken, it had nothing
     * to show. kf_time_init() seeds the simulated clock from
     * std::chrono::system_clock and kf_host_time_system_now() reads the
     * same source, so on a freshly launched simulator a sync is a genuine
     * no-op. Every OTHER control in the sleep row moves this clock, and
     * until now this window displayed none of it: the only clock on screen
     * was the small one in the pet window. A control whose effect is
     * invisible is indistinguishable from a broken one, which is exactly
     * how this got reported.
     *
     * The drift figure is the useful half -- it is what makes "Sync Clock
     * did something" legible (it goes to +0s) and what shows at a glance
     * how far a testing session has wandered. */
    {
        const kf_wall_time wall = kf_time_wall();
        if (!wall.valid) {
            std::snprintf(line, sizeof(line), "clock: never set");
        } else {
            kf_civil civil;
            kf_civil_from_epoch(wall.epoch_seconds, &civil);
            const long long drift =
                static_cast<long long>(wall.epoch_seconds -
                                        kf_host_time_system_now());
            std::snprintf(line, sizeof(line),
                          "clock: %04u-%02u-%02u %02u:%02u:%02u  (host %+lldm)",
                          static_cast<unsigned>(civil.year),
                          static_cast<unsigned>(civil.month),
                          static_cast<unsigned>(civil.day),
                          static_cast<unsigned>(civil.hour),
                          static_cast<unsigned>(civil.minute),
                          static_cast<unsigned>(civil.second),
                          drift / 60);
        }
        SDL_RenderDebugText(g.renderer, 16, y, line);
        y += kLineHeight;
    }

    std::snprintf(line, sizeof(line), "age: %llus / %llus (drag the timeline)",
                  static_cast<unsigned long long>(current_age),
                  static_cast<unsigned long long>(axis_max));
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    /* "(asleep)" suffix: cheap, and worth having here rather than a line of
     * its own, because it is the answer to a question the neglect block
     * below otherwise invites -- "why hasn't neglect moved in a while?"
     * apply_stage_segment() (hakoniwaos/src/pet.cpp) pauses the neglect
     * clock for whatever part of a segment falls both neglected AND inside
     * the 22:00-07:00 night window, so a neglect_seconds reading that looks
     * frozen while the creature is visibly asleep is expected behaviour,
     * not a stuck counter. */
    if (state->asleep) {
        std::snprintf(line, sizeof(line), "stage: %s (asleep)",
                      stage_name(state->stage));
    } else {
        std::snprintf(line, sizeof(line), "stage: %s", stage_name(state->stage));
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    if (duration > 0u) {
        std::snprintf(line, sizeof(line), "stage time: %llus / %llus",
                      static_cast<unsigned long long>(
                          state->stage_elapsed_seconds),
                      static_cast<unsigned long long>(duration));
    } else {
        std::snprintf(line, sizeof(line), "stage time: %llus (terminal)",
                      static_cast<unsigned long long>(
                          state->stage_elapsed_seconds));
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    if (state->stage >= KF_PET_STAGE_TEEN) {
        std::snprintf(line, sizeof(line), "teen form: %u",
                      static_cast<unsigned>(state->teen_form));
    } else {
        std::snprintf(line, sizeof(line), "teen form: - (not decided yet)");
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    if (state->stage == KF_PET_STAGE_ADULT) {
        std::snprintf(line, sizeof(line), "adult branch: %u",
                      static_cast<unsigned>(state->adult_branch));
    } else {
        std::snprintf(line, sizeof(line), "adult branch: - (not decided yet)");
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    /* Task 8: what the Form-N/Adult-N/stage-jump/Next Stage buttons above
     * are CURRENTLY set to use -- distinct from "teen form"/"adult branch"
     * above, which read the pet's own actual, already-decided state (and
     * show "- (not decided yet)" until it has one). This line always shows
     * a concrete pair (0/0 unless changed), because a jump always uses
     * one -- see kf_pet_session_debug_jump_to_stage()'s header comment on
     * why 0 is the fallback for both unset AND out-of-range. */
    std::snprintf(line, sizeof(line), "jump picks: teen %u / adult %u",
                  static_cast<unsigned>(g.selected_teen_form),
                  static_cast<unsigned>(g.selected_adult_branch));
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight * 1.5f;

    std::snprintf(line, sizeof(line), "hunger:   %u.%u%%",
                  state->hunger_mp / 1000u, (state->hunger_mp / 100u) % 10u);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "happy:    %u.%u%%",
                  state->happiness_mp / 1000u,
                  (state->happiness_mp / 100u) % 10u);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "energy:   %u.%u%%",
                  state->energy_mp / 1000u, (state->energy_mp / 100u) % 10u);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    /* Dirtiness and poop count: two of is_neglected()'s five inputs
     * (hakoniwaos/src/pet.cpp), and until now invisible on this window
     * entirely -- hunger/happy/energy were readable above, but a creature
     * could be marched into neglect purely through mess and this window
     * would show nothing that explained it. Shown against the SAME
     * threshold is_neglected() actually compares poop_count to
     * (neglect_poop_count), the same "value against what it is racing
     * toward" shape the age/stage-time lines above already use. Dirtiness's
     * own threshold (neglect_dirtiness_mp) is not repeated here the same
     * way -- it is always KF_PET_DIRTY_STINK_MP by default (kf/pet.h), the
     * same point the screen's own stink lines appear at, so a player who
     * has seen the stink lines already knows the line this bar is racing
     * toward. */
    std::snprintf(line, sizeof(line), "dirtiness: %u.%u%%   poops: %u/%u",
                  state->dirtiness_mp / 1000u,
                  (state->dirtiness_mp / 100u) % 10u,
                  static_cast<unsigned>(state->poop_count),
                  static_cast<unsigned>(config.neglect_poop_count));
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight * 1.5f;

    /* The death chain, made visible -- added after Chris's pet died and
     * this window gave him nothing to explain it with. Four lines: how much
     * neglect has accumulated and what it is racing toward (effective, not
     * raw -- see effective_sickness_seconds()'s own header comment for why
     * the raw config numbers alone would lie), WHICH of the five neglect
     * conditions is actually responsible right now, and the creature's
     * overall status with a concrete time-to-death when it applies. This is
     * everything a person watching this window needs to answer "why is my
     * pet dying, and how long have I got" without reading hakoniwaos/src/
     * pet.cpp themselves. */
    SDL_RenderDebugText(g.renderer, 16, y, "-- neglect & sickness --");
    y += kLineHeight;

    const bool have_clock = state->last_advanced.valid;
    const uint32_t effective_onset =
        effective_sickness_seconds(config.sickness_onset_seconds, have_clock);
    /* sickness_death_seconds == 0 is a documented sentinel (kf/pet.h) for
     * "this creature never dies of neglect" -- checked against the RAW
     * config value, exactly as apply_stage_segment() itself checks it
     * (pet.cpp), so a nonzero raw value that happens to compress to 0
     * cannot be misread as the sentinel by this window either. */
    const bool death_enabled = config.sickness_death_seconds > 0u;
    const uint32_t effective_death =
        effective_sickness_seconds(config.sickness_death_seconds, have_clock);

    /* A ratio, not a bare accumulating number -- Chris's own complaint
     * after the fact was that a raw neglect_seconds figure says nothing
     * about how much runway is left. `current/max` against the EFFECTIVE
     * death threshold (not the raw config value -- see this block's own
     * header comment on why the raw number would lie), both in h/m rather
     * than seconds, is the same "value against what it is racing toward"
     * shape the age/stage-time lines already use above, made legible as a
     * FELT duration instead of a number nobody can place. The onset point
     * gets the identical h/m treatment for the same reason: a mix of
     * "3h00m" and "10800s" on the same line would make the reader convert
     * one of them just to compare. */
    char neglect_hm[16];
    char onset_hm[16];
    format_hm(state->neglect_seconds, neglect_hm, sizeof(neglect_hm));
    format_hm(effective_onset, onset_hm, sizeof(onset_hm));
    if (death_enabled) {
        char death_hm[16];
        format_hm(effective_death, death_hm, sizeof(death_hm));
        std::snprintf(line, sizeof(line), "neglect %s / %s  (onset %s)%s",
                      neglect_hm, death_hm, onset_hm,
                      have_clock ? "" : "  (raw -- clock not set)");
    } else {
        std::snprintf(line, sizeof(line),
                      "neglect %s  (onset %s, death: never)%s", neglect_hm,
                      onset_hm, have_clock ? "" : "  (raw -- clock not set)");
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    char driver[96];
    describe_neglect_driver(driver, sizeof(driver), state, &config);
    std::snprintf(line, sizeof(line), "neglect driver: %s", driver);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    if (state->dead) {
        std::snprintf(line, sizeof(line), "status: DEAD");
    } else if (state->sick) {
        if (!death_enabled) {
            std::snprintf(line, sizeof(line),
                          "status: SICK -- death disabled (never)");
        } else if (state->neglect_seconds >= effective_death) {
            /* Reachable for exactly one frame: kf_pet_advance() sets `dead`
             * true the same segment neglect_seconds first reaches the
             * threshold (pet.cpp), so this window should show DEAD, above,
             * on the very next redraw. Kept as a real branch rather than
             * assumed unreachable -- see this codebase's own operator
             * rule about verifying "impossible" claims before relying on
             * them; a stale timing assumption here would show "SICK" on a
             * pet that is, functionally, already gone. */
            std::snprintf(line, sizeof(line),
                          "status: SICK -- time to death: 0h00m (this frame)");
        } else {
            const uint32_t remaining = effective_death - state->neglect_seconds;
            char remaining_hm[16];
            format_hm(remaining, remaining_hm, sizeof(remaining_hm));
            std::snprintf(line, sizeof(line),
                          "status: SICK -- time to death: %s", remaining_hm);
        }
    } else {
        std::snprintf(line, sizeof(line), "status: OK");
    }
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    /* History, not the present moment -- what killed the pet LAST time
     * (across every life this save directory has ever had), so the owner
     * does not need to catch the exact frame of death to find out why it
     * happened. Read from its own store key, never kf_pet_state -- see
     * kf_pet_death_cause's own header comment (kf_pet_session.h) for why
     * growing the pet save was rejected for this. */
    char last_died_of[96];
    describe_death_cause(last_died_of, sizeof(last_died_of),
                         kf_pet_session_last_death_cause());
    std::snprintf(line, sizeof(line), "last died of: %s", last_died_of);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight * 1.5f;

    std::snprintf(line, sizeof(line), "time multiplier: %ux", g.multiplier);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "screen: %s (Next Screen above, or MENU)",
                  kf_screen_nav_name(kf_screen_nav_debug_index()));
    SDL_RenderDebugText(g.renderer, 16, y, line);

    draw_engine_diagnostics();

    SDL_RenderPresent(g.renderer);
}

uint32_t kf_sdl_debug_window_time_multiplier(void) { return g.multiplier; }

void kf_sdl_debug_window_shutdown(void) {
    if (g.renderer != nullptr) {
        SDL_DestroyRenderer(g.renderer);
        g.renderer = nullptr;
    }
    if (g.window != nullptr) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
    kf_sdl_state().debug_window = nullptr;
}
