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
 * kf_audio_tone() generates the WHOLE buffer up front (sample_rate * ms /
 * 1000 samples) and hands it to SDL_PutAudioStreamData() in one call, then
 * returns immediately -- SDL's own audio thread drains it into the device
 * over the real duration, which is what makes this non-blocking the way
 * kf/hal/audio.h requires. A callback-driven streaming implementation would
 * work too and would use less memory for a long tone, but nothing this HAL
 * plays is longer than KF_AUDIO_MAX_MS (5s), and generate-then-hand-off is
 * far less code to get right for a five-second buffer at most.
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

SDL_AudioStream *g_stream = nullptr;

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

kf_result kf_audio_tone(uint32_t hz, uint32_t ms) {
    if (hz < KF_AUDIO_MIN_HZ || hz > KF_AUDIO_MAX_HZ) {
        return KF_ERR_INVALID;
    }
    if (ms == 0u || ms > KF_AUDIO_MAX_MS) {
        return KF_ERR_INVALID;
    }
    if (g_stream == nullptr) {
        return KF_ERR_UNAVAILABLE; /* no device -- see kf_audio_init() */
    }

    /* Square wave, integer period math -- no float anywhere in this HAL
     * (see kf/hal/audio.h's own comment on why that is a deliberate, hand-
     * kept convention here rather than only a hakoniwaos/ requirement).
     * half_period_samples toggling between +amplitude and -amplitude is
     * exactly what a buzzer's own GPIO-toggle waveform looks like once fed
     * through a speaker -- a duty-cycle-50% square wave, not a sine. */
    const uint32_t sample_count =
        static_cast<uint32_t>((static_cast<uint64_t>(kSampleRateHz) * ms) /
                               1000ull);
    const uint32_t half_period_samples =
        (static_cast<uint32_t>(kSampleRateHz) / hz) / 2u;
    if (half_period_samples == 0u || sample_count == 0u) {
        return KF_ERR_INVALID; /* hz too high for this sample rate to
                                 * represent even one full cycle -- cannot
                                 * happen with KF_AUDIO_MAX_HZ's own ceiling
                                 * at 44.1kHz, kept as a guard rather than an
                                 * assert since it costs nothing */
    }

    constexpr int16_t kAmplitude = 8000; /* headroom below INT16_MAX, so this
                                           * never clips even summed with
                                           * whatever else SDL happens to be
                                           * mixing */
    std::vector<int16_t> samples(sample_count);
    for (uint32_t i = 0; i < sample_count; ++i) {
        const bool high = (i / half_period_samples) % 2u == 0u;
        samples[i] = high ? kAmplitude : static_cast<int16_t>(-kAmplitude);
    }

    /* Replaces whatever is still queued, matching kf_audio_tone()'s own
     * documented "not queued behind it" contract. */
    SDL_ClearAudioStream(g_stream);
    if (!SDL_PutAudioStreamData(
            g_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(int16_t)))) {
        KF_LOGW(TAG, "SDL_PutAudioStreamData failed: %s", SDL_GetError());
        return KF_ERR_IO;
    }
    return KF_OK;
}

void kf_audio_stop(void) {
    if (g_stream == nullptr) {
        return;
    }
    SDL_ClearAudioStream(g_stream);
}

void kf_audio_shutdown(void) {
    if (g_stream != nullptr) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
