/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include <cstdint>

namespace {

constexpr const char *TAG = "pet-session";

/* How many seconds of a stage's own duration have already been lived
 * through as of `stage` starting -- i.e. where `stage` begins on the
 * pet's own lifetime clock. Adult has no duration of its own (it is
 * terminal, see kf/pet.h), so this is also the total axis length the
 * debug window's timeline draws tick marks across. Pure function of
 * config + stage, no session state involved, which is why this lives as
 * a free function rather than a Session member. */
uint64_t elapsed_before_stage(const kf_pet_config &config, kf_pet_stage stage) {
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

uint64_t total_age_seconds(const kf_pet_state &state, const kf_pet_config &config) {
    return elapsed_before_stage(config, state.stage) + state.stage_elapsed_seconds;
}

/* One full state snapshot, timestamped on the pet's own lifetime clock
 * (see total_age_seconds() above), not a wall-clock or session-uptime
 * reading. See kf_pet_session_debug_seek()'s header comment in
 * kf_pet_session.h for why snapshots, not an action-replay log: the
 * error this trades away (an approximated care_integral at the exact
 * moment of a stage transition) is bounded by snapshot SPACING, which
 * this file controls tightly (every state change), rather than by
 * however far apart a player's care-action clicks happen to land. */
struct DebugSnapshot {
    kf_pet_state state;
    uint64_t age_seconds;
};

/* 2048 * ~48 bytes is under 100KB -- desktop-simulator-only process
 * memory, nothing close to an arena budget (this file has none; see
 * kf_pet_session.h's own header comment on why this lives in
 * simulator/, not hakoniwaos/). Generous enough that a normal debug
 * session never evicts its own history; a sustained high-multiplier run
 * (see sdl_debug_window.cpp's 256x) can still fill it, at which point
 * the ring starts overwriting its OLDEST entries -- see
 * debug_snapshot_push() below. */
constexpr size_t kDebugSnapshotCapacity = 2048u;

struct Session {
    kf_pet_state state{};
    kf_pet_config config{};
    uint64_t last_call_us = 0;
    /* Elapsed milliseconds accumulated but not yet applied via
     * kf_pet_advance() -- can exceed 1000 while waiting to cross
     * KF_PET_SESSION_FLUSH_SECONDS. See kf_pet_session_frame()'s header
     * comment in kf_pet_session.h for why this batches rather than
     * flushing every whole second. */
    uint64_t pending_ms = 0;
    bool ready = false;

    /* Debug-only: the timeline snapshot ring. See kf_pet_session.h's
     * "DEBUG ONLY" section. Written by debug_snapshot_push() after every
     * state change; read only by kf_pet_session_debug_seek(). A plain
     * ring buffer, not a growable log -- oldest entries are silently
     * overwritten once full, exactly like any other fixed-capacity
     * structure this project uses (no heap in Core; this file is not
     * Core, but there is no reason to reach for unbounded growth here
     * either). */
    DebugSnapshot debug_snapshots[kDebugSnapshotCapacity];
    size_t debug_snapshot_count = 0;
    size_t debug_snapshot_next = 0;
};
Session g;

/* Called after every state-changing operation below (a live-tick flush,
 * a debug_advance(), a care action, or a reset) so
 * kf_pet_session_debug_seek() always has a snapshot at or before any age
 * it is asked to reach. Deliberately NOT called from seek() itself --
 * scrubbing is a preview, not a new history event; snapshotting every
 * seek would let a single drag gesture (dozens of seek() calls a
 * second) flood the ring and evict real history for a look that gets
 * thrown away the moment the user drags somewhere else. */
void debug_snapshot_push() {
    DebugSnapshot &slot = g.debug_snapshots[g.debug_snapshot_next];
    slot.state = g.state;
    slot.age_seconds = total_age_seconds(g.state, g.config);
    g.debug_snapshot_next = (g.debug_snapshot_next + 1u) % kDebugSnapshotCapacity;
    if (g.debug_snapshot_count < kDebugSnapshotCapacity) {
        g.debug_snapshot_count++;
    }
}

void debug_snapshot_reset() {
    g.debug_snapshot_count = 0;
    g.debug_snapshot_next = 0;
    debug_snapshot_push();
}

} // namespace

void kf_pet_session_init(void) {
    KF_ASSERT(!g.ready, "kf_pet_session_init called twice without an "
                        "intervening kf_pet_session_shutdown()");

    g.config = kf_pet_default_config();
    const kf_result result = kf_pet_load_and_advance(&g.state, &g.config);
    KF_ASSERT(result == KF_OK,
              "kf_pet_load_and_advance failed at boot (%d) -- a corrupt or "
              "missing save should fall back to a fresh pet, not fail this "
              "hard; if this fires, something in kf_pet_load_and_advance's "
              "own error handling regressed",
              static_cast<int>(result));

    g.last_call_us = 0;
    g.pending_ms = 0;
    g.ready = true;
    debug_snapshot_reset();
}

void kf_pet_session_frame(uint32_t synthetic_frame_delta_ms) {
    KF_ASSERT(g.ready,
              "kf_pet_session_frame called before kf_pet_session_init");

    uint32_t dt_ms = synthetic_frame_delta_ms;
    if (dt_ms == 0u) {
        const uint64_t now_us = kf_time_mono_us();
        dt_ms = g.last_call_us == 0u
                    ? 0u
                    : static_cast<uint32_t>(
                          (now_us >= g.last_call_us
                               ? now_us - g.last_call_us
                               : 0u) /
                          1000u);
        g.last_call_us = now_us;
    }

    g.pending_ms += dt_ms;

    constexpr uint64_t kFlushThresholdMs =
        static_cast<uint64_t>(KF_PET_SESSION_FLUSH_SECONDS) * 1000ull;
    if (g.pending_ms >= kFlushThresholdMs) {
        const uint32_t whole_seconds =
            static_cast<uint32_t>(g.pending_ms / 1000ull);
        kf_pet_advance(&g.state, &g.config, whole_seconds);
        g.pending_ms = g.pending_ms % 1000ull;
        debug_snapshot_push();
    }
}

const kf_pet_state *kf_pet_session_state(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_state called before kf_pet_session_init");
    return &g.state;
}

void kf_pet_session_feed(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_feed called before kf_pet_session_init");
    kf_pet_feed(&g.state);
    debug_snapshot_push();
}

void kf_pet_session_play(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_play called before kf_pet_session_init");
    kf_pet_play(&g.state);
    debug_snapshot_push();
}

void kf_pet_session_rest(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_rest called before kf_pet_session_init");
    kf_pet_rest(&g.state);
    debug_snapshot_push();
}

void kf_pet_session_save(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_save called before kf_pet_session_init");
    const kf_result result = kf_pet_save(&g.state);
    if (result != KF_OK) {
        KF_LOGE(TAG,
                "kf_pet_save failed (%d) -- will try again at the next "
                "checkpoint",
                static_cast<int>(result));
    }
}

void kf_pet_session_shutdown(void) {
    if (!g.ready) {
        return;
    }
    kf_pet_session_save();
    g.ready = false;
}

void kf_pet_session_debug_advance(uint32_t seconds) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_advance called before "
                        "kf_pet_session_init");
    kf_pet_advance(&g.state, &g.config, seconds);
    debug_snapshot_push();
}

void kf_pet_session_debug_reset(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_debug_reset called before kf_pet_session_init");
    kf_pet_init(&g.state);
    g.pending_ms = 0;
    debug_snapshot_reset();
}

uint64_t kf_pet_session_debug_age_seconds(void) {
    KF_ASSERT(g.ready, "kf_pet_session_debug_age_seconds called before "
                        "kf_pet_session_init");
    return total_age_seconds(g.state, g.config);
}

void kf_pet_session_debug_seek(uint64_t target_age_seconds) {
    KF_ASSERT(g.ready,
              "kf_pet_session_debug_seek called before kf_pet_session_init");
    if (g.debug_snapshot_count == 0u) {
        /* Can't happen in practice -- kf_pet_session_init() always seeds
         * one snapshot -- but a seek with nothing to seek from is a no-op,
         * not a crash. */
        return;
    }

    /* Find the snapshot with the largest age at or before the target; if
     * the target is earlier than every surviving snapshot (evicted, or
     * before this pet's own genesis), fall back to the EARLIEST surviving
     * one instead -- see kf_pet_session.h's header comment on this
     * clamping behaviour. One linear scan of at most kDebugSnapshotCapacity
     * plain-old-data entries, called at most once per frame even while
     * actively dragging the timeline -- not worth a sorted structure. */
    const DebugSnapshot *best_at_or_before = nullptr;
    const DebugSnapshot *earliest = nullptr;
    for (size_t i = 0; i < g.debug_snapshot_count; ++i) {
        const DebugSnapshot &snap = g.debug_snapshots[i];
        if (snap.age_seconds <= target_age_seconds &&
            (best_at_or_before == nullptr ||
             snap.age_seconds > best_at_or_before->age_seconds)) {
            best_at_or_before = &snap;
        }
        if (earliest == nullptr || snap.age_seconds < earliest->age_seconds) {
            earliest = &snap;
        }
    }
    const DebugSnapshot *base = best_at_or_before != nullptr ? best_at_or_before : earliest;

    kf_pet_state replay = base->state;
    if (target_age_seconds > base->age_seconds) {
        const uint64_t delta = target_age_seconds - base->age_seconds;
        const uint32_t delta_seconds =
            delta > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(delta);
        kf_pet_advance(&replay, &g.config, delta_seconds);
    }
    g.state = replay;

    /* A backward or forward jump makes whatever fractional live-tick
     * remainder was pending before this seek meaningless -- it was
     * measured against the pre-seek position. Drop it rather than let it
     * apply a small (< KF_PET_SESSION_FLUSH_SECONDS) phantom nudge on top
     * of wherever the seek just landed, the instant normal play resumes. */
    g.pending_ms = 0;
}
