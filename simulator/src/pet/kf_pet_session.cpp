/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 */

#include "kf_pet_session.h"

#include "kf/hal/log.h"
#include "kf/hal/time.h"

#include <cstdint>

namespace {

constexpr const char *TAG = "pet-session";

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
};
Session g;

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
}

void kf_pet_session_play(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_play called before kf_pet_session_init");
    kf_pet_play(&g.state);
}

void kf_pet_session_rest(void) {
    KF_ASSERT(g.ready,
              "kf_pet_session_rest called before kf_pet_session_init");
    kf_pet_rest(&g.state);
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
