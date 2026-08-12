/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: audio, headless.
 *
 * Makes no sound. Records what it was ASKED to do -- exactly the same "draws
 * nowhere, keeps a checksum" shape headless_display.cpp already established
 * for the display HAL, applied to audio: two jobs.
 *
 *   1. CI can run the real firmware, with the real Lua binding and the real
 *      attention-signal chirp, and assert on what would have played. No
 *      speaker, no audio device, no CI runner accidentally making noise in
 *      someone's open-plan office at 3am.
 *
 *   2. It proves the backend swap works, the same way it does for display:
 *      finding out now that a tone request reaches this HAL cleanly is much
 *      better than finding out during hardware bring-up.
 *
 * MUST STAY SILENT AND FAST (this file's own part of the brief): no SDL, no
 * device, no sleep -- kf_audio_tone() below does nothing but validate and
 * record, in microseconds, the same as every other headless HAL call in
 * this directory.
 */

#include "kf/hal/audio.h"

#include "kf/hal/log.h"
#include "headless_probe.h"

namespace {

constexpr const char *TAG = "audio";

uint64_t g_tone_count = 0;
uint32_t g_last_hz = 0;
uint32_t g_last_ms = 0;
uint64_t g_stop_count = 0;

} // namespace

kf_result kf_audio_init(void) {
    KF_LOGI(TAG, "headless: recording only, no sound");
    return KF_OK;
}

kf_result kf_audio_tone(uint32_t hz, uint32_t ms) {
    if (hz < KF_AUDIO_MIN_HZ || hz > KF_AUDIO_MAX_HZ) {
        return KF_ERR_INVALID;
    }
    if (ms == 0u || ms > KF_AUDIO_MAX_MS) {
        return KF_ERR_INVALID;
    }
    g_tone_count++;
    g_last_hz = hz;
    g_last_ms = ms;
    return KF_OK;
}

void kf_audio_stop(void) { g_stop_count++; }

void kf_audio_shutdown(void) {}

uint64_t kf_headless_audio_tone_count(void) { return g_tone_count; }
uint32_t kf_headless_audio_last_hz(void) { return g_last_hz; }
uint32_t kf_headless_audio_last_ms(void) { return g_last_ms; }
uint64_t kf_headless_audio_stop_count(void) { return g_stop_count; }

void kf_headless_audio_reset(void) {
    g_tone_count = 0;
    g_last_hz = 0;
    g_last_ms = 0;
    g_stop_count = 0;
}
