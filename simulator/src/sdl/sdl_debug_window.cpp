/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "sdl_debug_window.h"

#include "sdl_shared.h"

#include "../lvgl/kf_screen_nav.h"
#include "../pet/kf_pet_session.h"

#include "kf/app.h"
#include "kf/arena.h"
#include "kf/hal/log.h"
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
constexpr int kWindowH = 670;
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
     * slots (KF_PET_TEEN_FORM_COUNT) and three adult_branch slots
     * (KF_PET_ADULT_BRANCH_MAX, the widest family) -- see kf/pet.h. An
     * adult_branch picked here that is out of range for whichever
     * teen_form is ALSO currently selected is not rejected here (this
     * window does not know each family's exact count without calling
     * kf_pet_adults_in_family(), which it happily could, but the session
     * layer already clamps it defensively -- see kf_pet_session_debug_
     * jump_to_stage()'s own header comment), so pressing e.g. "Adult 2"
     * against a one-adult family is harmless, just not useful. */
    {{16, 176, 64, 32}, "Form0", DebugAction::kTeenForm0},
    {{88, 176, 64, 32}, "Form1", DebugAction::kTeenForm1},
    {{160, 176, 64, 32}, "Form2", DebugAction::kTeenForm2},
    {{232, 176, 64, 32}, "Form3", DebugAction::kTeenForm3},

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

/* kf_screen_nav.h's own header comment documents index 0 = Home, 1 = Info
 * -- a small, deliberate duplication of that contract rather than a new
 * export, the same call this file already made for stage_name() above.
 * "?" covers a future screen this file has not been updated to name yet,
 * rather than showing a raw, meaningless number. */
const char *screen_name(int index) {
    switch (index) {
    case 0:
        return "Home";
    case 1:
        return "Info";
    default:
        return "?";
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
 * see kButtons and kWindowH's own comments above). */
constexpr SDL_FRect kTimelineBar = {16, 344, kLeftColumnW - 32, 14};
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
           action == DebugAction::kTeenForm3;
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

void perform(DebugAction action) {
    switch (action) {
    case DebugAction::kSkipHour:
        kf_pet_session_debug_advance(3600u);
        break;
    case DebugAction::kSkipDay:
        kf_pet_session_debug_advance(24u * 3600u);
        break;
    case DebugAction::kSkipWeek:
        kf_pet_session_debug_advance(7u * 24u * 3600u);
        break;
    case DebugAction::kReset:
        kf_pet_session_debug_reset();
        break;
    case DebugAction::kSave:
        kf_pet_session_save();
        break;
    case DebugAction::kNextScreen:
        /* Same effect a real MENU press has on which pet-window screen is
         * loaded -- see kf_screen_nav.h -- but calling this directly
         * means kf_app_frame() never sees a KF_BTN_MENU edge, so Core's
         * on-device HUD toggle (kf/app.cpp) never fires either. That is
         * the entire point of this button; see this file's own header
         * comment. */
        kf_screen_nav_debug_advance();
        break;
    case DebugAction::kJumpNextStage: {
        /* "I want a button to just autoprogress" -- one press, one stage
         * forward from wherever the pet currently is. Adult is terminal
         * (kf/pet.h), so once already there, "the following stage" is
         * Adult itself: this still refills needs and clears sickness/
         * neglect/mess via the jump (kf_pet_session_debug_jump_to_stage()
         * always starts from kf_pet_init()), it just does not move the
         * marker further along the timeline -- there is nowhere further
         * for it to go, the same terminal status kf_pet_advance() itself
         * already enforces. */
        const kf_pet_stage current = kf_pet_session_state()->stage;
        const kf_pet_stage next = current < KF_PET_STAGE_ADULT
                                       ? static_cast<kf_pet_stage>(
                                             static_cast<int>(current) + 1)
                                       : KF_PET_STAGE_ADULT;
        kf_pet_session_debug_jump_to_stage(next, g.selected_teen_form,
                                            g.selected_adult_branch);
        break;
    }
    default:
        if (is_multiplier_button(action)) {
            g.multiplier = multiplier_for(action);
        } else if (is_teen_form_button(action)) {
            g.selected_teen_form = teen_form_for(action);
        } else if (is_adult_branch_button(action)) {
            g.selected_adult_branch = adult_branch_for(action);
        } else if (is_jump_stage_button(action)) {
            kf_pet_session_debug_jump_to_stage(
                jump_stage_for(action), g.selected_teen_form,
                g.selected_adult_branch);
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

    /* All five arenas, unlike the on-device HUD's four (kf/app.cpp's
     * draw_hud() predates KF_ARENA_LVGL -- see kf/arena.h) -- nothing
     * about this column is squeezed for space the way the real HUD is, so
     * there is no reason to leave one out. */
    SDL_RenderDebugText(g.renderer, kRightColumnX, y, "-- arenas (hi/cap KB) --");
    y += kLineHeight;

    constexpr kf_arena_id kArenas[] = {KF_ARENA_FRAMEBUFFER, KF_ARENA_SCRATCH,
                                       KF_ARENA_LUA, KF_ARENA_ASSETS,
                                       KF_ARENA_LVGL};
    for (kf_arena_id arena : kArenas) {
        const kf_arena_stats *s = kf_arena_get_stats(arena);
        std::snprintf(line, sizeof(line), "%-11s %u/%uK", s->name,
                      static_cast<unsigned>(s->high_water_bytes / 1024u),
                      static_cast<unsigned>(s->capacity_bytes / 1024u));
        SDL_RenderDebugText(g.renderer, kRightColumnX, y, line);
        y += kLineHeight;
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
    float y = 400.0f;
    constexpr float kLineHeight = 18.0f;
    SDL_SetRenderDrawColor(g.renderer, 220, 220, 225, 255);

    std::snprintf(line, sizeof(line), "age: %llus / %llus (drag the timeline)",
                  static_cast<unsigned long long>(current_age),
                  static_cast<unsigned long long>(axis_max));
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "stage: %s", stage_name(state->stage));
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
    y += kLineHeight * 1.5f;

    std::snprintf(line, sizeof(line), "time multiplier: %ux", g.multiplier);
    SDL_RenderDebugText(g.renderer, 16, y, line);
    y += kLineHeight;

    std::snprintf(line, sizeof(line), "screen: %s (Next Screen above, or MENU)",
                  screen_name(kf_screen_nav_debug_index()));
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
