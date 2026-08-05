/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "sdl_debug_window.h"

#include "sdl_shared.h"

#include "../pet/kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/pet.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace {

constexpr const char *TAG = "debug-window";

constexpr int kWindowW = 400;
constexpr int kWindowH = 460;

enum class DebugAction {
    kSkipHour,
    kSkipDay,
    kSkipWeek,
    kReset,
    kSave,
    kMult1,
    kMult2,
    kMult4,
    kMult8,
    kMult16,
    kMult32,
    kMult64,
    kMult128,
    kMult256,
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

    {{16, 96, 64, 32}, "1x", DebugAction::kMult1},
    {{88, 96, 64, 32}, "2x", DebugAction::kMult2},
    {{160, 96, 64, 32}, "4x", DebugAction::kMult4},
    {{232, 96, 64, 32}, "8x", DebugAction::kMult8},
    {{304, 96, 64, 32}, "16x", DebugAction::kMult16},

    {{16, 136, 64, 32}, "32x", DebugAction::kMult32},
    {{88, 136, 64, 32}, "64x", DebugAction::kMult64},
    {{160, 136, 64, 32}, "128x", DebugAction::kMult128},
    {{232, 136, 64, 32}, "256x", DebugAction::kMult256},
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

constexpr SDL_FRect kTimelineBar = {16, 200, kWindowW - 32, 14};
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
    default:
        if (is_multiplier_button(action)) {
            g.multiplier = multiplier_for(action);
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
    const bool active = is_multiplier_button(b.action) &&
                         multiplier_for(b.action) == g.multiplier;
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
    float y = 250.0f;
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
