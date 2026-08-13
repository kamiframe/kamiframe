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
 *
 * Two independent recording surfaces, matching the two independent entry
 * points kf/hal/audio.h now exposes: kf_audio_tone()/kf_audio_tone_duty()
 * (a single note) record into g_tone_*, kf_audio_play_notes() (a whole
 * sequence, kf.melody()'s own primitive) records into g_melody_*. They are
 * NOT layered on top of each other here -- unlike the SDL/ESP32 backends,
 * which genuinely synthesise a waveform and so share one implementation
 * between the two entry points to avoid duplicating that work, this backend
 * does no synthesis at all, and keeping the two recordings separate is what
 * lets run_audio_check()'s existing kf_audio_tone() assertions (Sections A/
 * B/C, from before kf_audio_play_notes() existed) keep working completely
 * unchanged. */

#include "kf/hal/audio.h"

#include "kf/hal/log.h"
#include "headless_probe.h"

#include <cstring>

namespace {

constexpr const char *TAG = "audio";

uint64_t g_tone_count = 0;
uint32_t g_last_hz = 0;
uint32_t g_last_ms = 0;
uint32_t g_last_tone_duty = 0;
uint64_t g_stop_count = 0;

uint64_t g_melody_count = 0;
uint32_t g_last_melody_note_count = 0;
uint32_t g_last_melody_duty = 0;
kf_audio_note g_last_melody_notes[KF_AUDIO_MAX_NOTES] = {};

/* KF_VOLUME_4 -- see kf_audio_get_volume()'s own header comment (kf/hal/
 * audio.h) for why that, not KF_VOLUME_OFF, is the process-start default. */
kf_volume_level g_volume = KF_VOLUME_4;

/* How many otherwise-valid kf_audio_tone()/_tone_duty()/kf_audio_play_
 * notes() calls were silenced by KF_VOLUME_OFF -- NEITHER g_tone_count NOR
 * g_melody_count increments on that path, and last_hz/last_ms/the melody
 * fields are left exactly as they were, which is what makes "off" provably
 * silent to a test rather than merely unobserved: a muted call leaves no
 * trace in the "what played" counters at all, only in this one. */
uint64_t g_muted_count = 0;

bool notes_valid(const kf_audio_note *notes, uint32_t count,
                  uint32_t duty_permille) {
    if (notes == nullptr || count == 0u || count > KF_AUDIO_MAX_NOTES) {
        return false;
    }
    if (duty_permille < KF_AUDIO_DUTY_MIN || duty_permille > KF_AUDIO_DUTY_MAX) {
        return false;
    }
    uint64_t total_ms = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (notes[i].ms == 0u) {
            return false;
        }
        if (notes[i].hz != 0u &&
            (notes[i].hz < KF_AUDIO_MIN_HZ || notes[i].hz > KF_AUDIO_MAX_HZ)) {
            return false;
        }
        total_ms += notes[i].ms;
    }
    if (total_ms > KF_AUDIO_MAX_SEQUENCE_MS) {
        return false;
    }
    return true;
}

} // namespace

kf_result kf_audio_init(void) {
    KF_LOGI(TAG, "headless: recording only, no sound");
    return KF_OK;
}

kf_result kf_audio_tone(uint32_t hz, uint32_t ms) {
    return kf_audio_tone_duty(hz, ms, KF_AUDIO_DUTY_FAT);
}

kf_result kf_audio_tone_duty(uint32_t hz, uint32_t ms, uint32_t duty_permille) {
    if (hz < KF_AUDIO_MIN_HZ || hz > KF_AUDIO_MAX_HZ) {
        return KF_ERR_INVALID;
    }
    if (ms == 0u || ms > KF_AUDIO_MAX_MS) {
        return KF_ERR_INVALID;
    }
    if (duty_permille < KF_AUDIO_DUTY_MIN || duty_permille > KF_AUDIO_DUTY_MAX) {
        return KF_ERR_INVALID;
    }
    if (g_volume == KF_VOLUME_OFF) {
        g_muted_count++;
        return KF_OK; /* genuinely silenced -- see g_muted_count's own
                        * comment for why g_tone_count/last_hz/last_ms are
                        * deliberately left untouched on this path */
    }
    g_tone_count++;
    g_last_hz = hz;
    g_last_ms = ms;
    g_last_tone_duty = duty_permille;
    return KF_OK;
}

kf_result kf_audio_play_notes(const kf_audio_note *notes, uint32_t count,
                               uint32_t duty_permille) {
    if (!notes_valid(notes, count, duty_permille)) {
        return KF_ERR_INVALID;
    }
    if (g_volume == KF_VOLUME_OFF) {
        g_muted_count++;
        return KF_OK;
    }
    g_melody_count++;
    g_last_melody_note_count = count;
    g_last_melody_duty = duty_permille;
    std::memcpy(g_last_melody_notes, notes, count * sizeof(kf_audio_note));
    return KF_OK;
}

void kf_audio_stop(void) { g_stop_count++; }

void kf_audio_set_volume(kf_volume_level level) {
    uint32_t clamped = static_cast<uint32_t>(level);
    if (clamped > static_cast<uint32_t>(KF_VOLUME_4)) {
        clamped = static_cast<uint32_t>(KF_VOLUME_4);
    }
    g_volume = static_cast<kf_volume_level>(clamped);
    kf_audio_stop(); /* a volume change silences whatever is currently
                       * sounding -- see this function's own header comment
                       * (kf/hal/audio.h) */
}

kf_volume_level kf_audio_get_volume(void) { return g_volume; }

void kf_audio_shutdown(void) {}

uint64_t kf_headless_audio_tone_count(void) { return g_tone_count; }
uint32_t kf_headless_audio_last_hz(void) { return g_last_hz; }
uint32_t kf_headless_audio_last_ms(void) { return g_last_ms; }
uint32_t kf_headless_audio_last_tone_duty(void) { return g_last_tone_duty; }
uint64_t kf_headless_audio_stop_count(void) { return g_stop_count; }

uint64_t kf_headless_audio_melody_count(void) { return g_melody_count; }
uint32_t kf_headless_audio_last_melody_note_count(void) {
    return g_last_melody_note_count;
}
uint32_t kf_headless_audio_last_melody_duty(void) {
    return g_last_melody_duty;
}
uint32_t kf_headless_audio_last_melody_note_hz(uint32_t index) {
    return index < g_last_melody_note_count ? g_last_melody_notes[index].hz
                                             : 0u;
}
uint32_t kf_headless_audio_last_melody_note_ms(uint32_t index) {
    return index < g_last_melody_note_count ? g_last_melody_notes[index].ms
                                             : 0u;
}

uint64_t kf_headless_audio_muted_count(void) { return g_muted_count; }

void kf_headless_audio_reset(void) {
    g_tone_count = 0;
    g_last_hz = 0;
    g_last_ms = 0;
    g_last_tone_duty = 0;
    g_stop_count = 0;
    g_melody_count = 0;
    g_last_melody_note_count = 0;
    g_last_melody_duty = 0;
    std::memset(g_last_melody_notes, 0, sizeof(g_last_melody_notes));
    g_muted_count = 0;
    /* g_volume is deliberately NOT reset here -- it is a persistent DEVICE
     * setting (kf/hal/audio.h's own kf_audio_set_volume() contract), not a
     * per-section recording counter, so a section that changed it should
     * not have that change silently undone by the next section's reset
     * call. Tests that care about volume set it explicitly. */
}
