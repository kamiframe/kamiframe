/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: audio, SDL3.
 *
 * The only backend in this HAL that is actually meant to be heard. SDL3's
 * audio stream API (SDL_OpenAudioDeviceStream et al.) does the mixing and
 * device I/O; this file's only job is to synthesise a square wave -- the
 * exact waveform a passive buzzer or a GPIO toggle produces, so a chirp
 * heard on Chris's Mac during development sounds like the same chirp a
 * buzzer or an I2S amp fed the same samples would make on the device, not
 * a nicer desktop-only substitute. Same reasoning sdl_display.cpp gives for
 * rendering the exact RGB565 bytes rather than converting to something that
 * looks better on a monitor.
 *
 * kf_audio_play_notes() generates the WHOLE sequence's buffer up front
 * (every note's samples, back to back, plus a short silence gap between
 * notes -- see kNoteGapMs below) and hands it to SDL_PutAudioStreamData()
 * in one call, then returns immediately -- SDL's own audio thread drains it
 * into the device over the real duration, which is what makes this non-
 * blocking the way kf/hal/audio.h requires. A callback-driven streaming
 * implementation would work too and would use less memory for a long
 * sequence, but nothing this HAL plays exceeds KF_AUDIO_MAX_SEQUENCE_MS
 * (5s), so generate-then-hand-off is far less code to get right.
 * kf_audio_tone()/kf_audio_tone_duty() are one-note calls into this same
 * function -- see kf_audio_tone_duty() below -- so there is exactly one
 * waveform generator in this file, not two to keep in sync.
 *
 * Heap-allocating (std::vector) is fine here: this file is a desktop
 * backend, not hakoniwaos/ core -- tools/check_no_heap.py does not scan it,
 * and unlike a buzzer or an I2S amp, a desktop process has a heap to spare.
 */

#include "kf/hal/audio.h"

#include "kf/hal/log.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace {

constexpr const char *TAG = "audio";

/* Fixed sample rate for the one stream this backend ever opens. 44.1kHz is
 * the ordinary desktop default and comfortably oversamples anything up to
 * KF_AUDIO_MAX_HZ (20kHz, itself already past most adults' hearing) --
 * nothing here needs a configurable rate since nothing in this HAL ever
 * asks for one. */
constexpr int kSampleRateHz = 44100;

constexpr int16_t kAmplitude = 8000; /* headroom below INT16_MAX, so this
                                       * never clips even summed with
                                       * whatever else SDL happens to be
                                       * mixing */

/* Silence between consecutive notes of one sequence, matching tools/
 * kf_chiptune.py's own render() `gap_ms=8` default exactly -- so a phrase
 * heard here sounds like the same phrase that tool's preview .wav plays,
 * not a backend-specific approximation of it. Not applied after the LAST
 * note (nothing follows it to separate from). */
constexpr uint32_t kNoteGapMs = 8;

/* The linear release ramp at the tail of every note, matching kf_chiptune.
 * py's own square()'s 2ms fade -- purely to stop the click a hard
 * amplitude cut makes on a small speaker, same reasoning that function's
 * own comment gives. */
constexpr uint32_t kReleaseMs = 2;

SDL_AudioStream *g_stream = nullptr;

/* KF_VOLUME_4 -- see kf_audio_get_volume()'s own header comment (kf/hal/
 * audio.h) for why that, not KF_VOLUME_OFF, is the process-start default. */
kf_volume_level g_volume = KF_VOLUME_4;

/* KF_VOLUME_1..4 map onto 1/4, 2/4, 3/4, 4/4 of full amplitude -- an even
 * split across the four non-off levels, not a tuned loudness curve; Chris's
 * to reshape (e.g. toward a perceptual/logarithmic curve) once he has heard
 * all four on the board. KF_VOLUME_OFF is handled separately (kf_audio_
 * play_notes() below skips producing output entirely rather than reaching
 * this table with a 0), so it has no entry here. */
uint32_t volume_gain_permille(kf_volume_level level) {
    switch (level) {
    case KF_VOLUME_1:
        return 250u;
    case KF_VOLUME_2:
        return 500u;
    case KF_VOLUME_3:
        return 750u;
    case KF_VOLUME_4:
    default:
        return 1000u;
    }
}

/* Appends one note's samples (a rest, hz == 0, is silence) plus the tail
 * ramp and the inter-note gap to `out`. Integer period math throughout --
 * no float anywhere in this HAL (see kf/hal/audio.h's own comment on why
 * that is a deliberate, hand-kept convention here rather than only a
 * hakoniwaos/ requirement). `duty_permille` is the fraction of each period
 * spent "high", the same general shape kf_chiptune.py's own square()
 * takes as its `duty` parameter, expressed in permille rather than a
 * fraction to stay integer-only. `gain_permille` scales the amplitude --
 * kf_audio_set_volume()'s own effect, applied here rather than at the
 * caller so every one of this HAL's three entry points obeys the current
 * volume with no extra call site to remember.
 *
 * Returns false if `hz` is non-zero and too high for this sample rate to
 * represent even one full cycle -- cannot happen with KF_AUDIO_MAX_HZ's own
 * ceiling at 44.1kHz, kept as a guard rather than an assert since it costs
 * nothing. */
bool append_note(std::vector<int16_t> &out, uint32_t hz, uint32_t ms,
                  uint32_t duty_permille, uint32_t gain_permille) {
    const uint32_t sample_count = static_cast<uint32_t>(
        (static_cast<uint64_t>(kSampleRateHz) * ms) / 1000ull);
    if (sample_count == 0u) {
        return false;
    }
    const size_t base = out.size();
    out.resize(base + sample_count);

    if (hz == 0u) {
        /* A rest: already zero-filled by std::vector::resize(). */
        for (size_t i = 0; i < sample_count; ++i) {
            out[base + i] = 0;
        }
    } else {
        const uint32_t period_samples =
            static_cast<uint32_t>(kSampleRateHz) / hz;
        if (period_samples == 0u) {
            out.resize(base); /* undo the reserve above */
            return false;
        }
        uint32_t on_samples =
            (period_samples * duty_permille) / 1000u;
        if (on_samples == 0u) {
            on_samples = 1u; /* never a fully-flat line at a valid duty */
        } else if (on_samples >= period_samples) {
            on_samples = period_samples - 1u;
        }
        const uint32_t ramp_samples = static_cast<uint32_t>(
            (static_cast<uint64_t>(kSampleRateHz) * kReleaseMs) / 1000ull);
        const int32_t peak = (static_cast<int32_t>(kAmplitude) *
                              static_cast<int32_t>(gain_permille)) /
                             1000;
        for (uint32_t i = 0; i < sample_count; ++i) {
            const bool high = (i % period_samples) < on_samples;
            int32_t v = high ? peak : -peak;
            const uint32_t tail = sample_count - i;
            if (ramp_samples > 0u && tail < ramp_samples) {
                v = (v * static_cast<int32_t>(tail)) /
                    static_cast<int32_t>(ramp_samples);
            }
            out[base + i] = static_cast<int16_t>(v);
        }
    }

    const uint32_t gap_samples = static_cast<uint32_t>(
        (static_cast<uint64_t>(kSampleRateHz) * kNoteGapMs) / 1000ull);
    out.resize(out.size() + gap_samples, 0);
    return true;
}

} // namespace

kf_result kf_audio_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        /* Matches this HAL's own "never a crash" contract: a dev machine
         * with no audio device (a CI runner, a headless VM someone tried
         * kamiframe-sim on anyway) gets silence, not a startup failure. */
        KF_LOGW(TAG, "SDL_InitSubSystem(AUDIO) failed: %s -- audio disabled",
                SDL_GetError());
        return KF_OK;
    }

    const SDL_AudioSpec spec{SDL_AUDIO_S16, /* channels */ 1, kSampleRateHz};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, nullptr, nullptr);
    if (g_stream == nullptr) {
        KF_LOGW(TAG, "SDL_OpenAudioDeviceStream failed: %s -- audio disabled",
                SDL_GetError());
        return KF_OK;
    }
    if (!SDL_ResumeAudioStreamDevice(g_stream)) {
        KF_LOGW(TAG, "SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
    }
    KF_LOGI(TAG, "SDL3 audio: %d Hz mono", kSampleRateHz);
    return KF_OK;
}

kf_result kf_audio_play_notes(const kf_audio_note *notes, uint32_t count,
                               uint32_t duty_permille) {
    if (notes == nullptr || count == 0u || count > KF_AUDIO_MAX_NOTES) {
        return KF_ERR_INVALID;
    }
    if (duty_permille < KF_AUDIO_DUTY_MIN || duty_permille > KF_AUDIO_DUTY_MAX) {
        return KF_ERR_INVALID;
    }
    uint64_t total_ms = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (notes[i].ms == 0u) {
            return KF_ERR_INVALID;
        }
        if (notes[i].hz != 0u &&
            (notes[i].hz < KF_AUDIO_MIN_HZ || notes[i].hz > KF_AUDIO_MAX_HZ)) {
            return KF_ERR_INVALID;
        }
        total_ms += notes[i].ms;
    }
    if (total_ms > KF_AUDIO_MAX_SEQUENCE_MS) {
        return KF_ERR_INVALID;
    }
    if (g_stream == nullptr) {
        return KF_ERR_UNAVAILABLE; /* no device -- see kf_audio_init() */
    }
    if (g_volume == KF_VOLUME_OFF) {
        /* GENUINELY silent: SDL_PutAudioStreamData() is never called at
         * all, no buffer is even synthesised -- see kf_audio_set_volume()'s
         * own header comment (kf/hal/audio.h) for why "off" means this,
         * not a zero-amplitude buffer. */
        return KF_OK;
    }

    const uint32_t gain_permille = volume_gain_permille(g_volume);
    std::vector<int16_t> samples;
    samples.reserve(static_cast<size_t>(
        (static_cast<uint64_t>(kSampleRateHz) * (total_ms + count * kNoteGapMs)) /
        1000ull));
    for (uint32_t i = 0; i < count; ++i) {
        if (!append_note(samples, notes[i].hz, notes[i].ms, duty_permille,
                          gain_permille)) {
            return KF_ERR_INVALID; /* cannot happen within KF_AUDIO_MAX_HZ's
                                     * own ceiling at 44.1kHz, kept as a
                                     * guard rather than an assert */
        }
    }

    /* Replaces whatever is still queued, matching kf_audio_play_notes()'s
     * own documented "not queued behind it" contract -- interrupts a
     * sequence still playing, does not wait for it to finish. */
    SDL_ClearAudioStream(g_stream);
    if (!SDL_PutAudioStreamData(
            g_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(int16_t)))) {
        KF_LOGW(TAG, "SDL_PutAudioStreamData failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }
    return KF_OK;
}

kf_result kf_audio_tone_duty(uint32_t hz, uint32_t ms, uint32_t duty_permille) {
    if (hz == 0u) {
        return KF_ERR_INVALID; /* hz == 0 is a caller mistake for a single
                                 * tone, unlike a rest inside a sequence --
                                 * see kf/hal/audio.h's own comment on
                                 * kf_audio_tone() vs kf_audio_note::hz */
    }
    const kf_audio_note note{hz, ms};
    return kf_audio_play_notes(&note, 1u, duty_permille);
}

kf_result kf_audio_tone(uint32_t hz, uint32_t ms) {
    return kf_audio_tone_duty(hz, ms, KF_AUDIO_DUTY_FAT);
}

void kf_audio_stop(void) {
    if (g_stream == nullptr) {
        return;
    }
    SDL_ClearAudioStream(g_stream);
}

void kf_audio_set_volume(kf_volume_level level) {
    uint32_t clamped = static_cast<uint32_t>(level);
    if (clamped > static_cast<uint32_t>(KF_VOLUME_4)) {
        clamped = static_cast<uint32_t>(KF_VOLUME_4);
    }
    g_volume = static_cast<kf_volume_level>(clamped);
    kf_audio_stop();
}

kf_volume_level kf_audio_get_volume(void) { return g_volume; }

void kf_audio_shutdown(void) {
    if (g_stream != nullptr) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
