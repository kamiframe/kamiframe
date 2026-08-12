/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: audio.
 *
 * ONE CAPABILITY: a square-wave tone at a frequency, for a caller-chosen
 * duration. Not sampled playback.
 *
 * The target hardware has two genuinely different ways to make sound -- a
 * MAX98357A I2S class-D amplifier (real sampled audio, any waveform) and a
 * passive buzzer (a GPIO toggled at an audio frequency, nothing more). This
 * header deliberately exposes only the buzzer's capability, not the amp's
 * full one, for one reason: `ports/esp32/hal/kf_esp_pins.h`'s own comment on
 * its I2S pins says outright that the passive buzzer this project owns "is
 * not given a pin at all" because it is "redundant with the amplifier" --
 * there is no separate buzzer GPIO on this board, so a tone is what the amp
 * gets asked to do too, by feeding it a synthesised square wave. A
 * kf_audio_play_pcm(sample_buffer, ...) call would be honest about what the
 * amp CAN do, but nothing in this repo can produce sample data for it yet --
 * there is no audio asset pipeline, no packer, nothing to author a clip
 * with -- so that call would exist to satisfy one backend, with every other
 * backend (buzzer, headless, an amp with nothing plugged into its DIN)
 * implementing it as "cannot". Building that surface now would be building
 * ahead of what anything can use, which is exactly what CLAUDE.md's
 * hardware section says not to do. A tone is the floor every one of these
 * backends can genuinely satisfy: a buzzer directly, an I2S amp by
 * synthesising the waveform in software (see ports/esp32/hal/esp_audio.cpp),
 * a desktop speaker via SDL3's audio API, and "nothing wired" by doing
 * nothing. When real sampled audio earns its own asset format and packer,
 * it earns its own HAL call then, not a flag bolted onto this one.
 *
 * Frequencies are integer Hz. Durations are integer milliseconds. No floats,
 * matching every other HAL header -- this file has no floating-point math of
 * its own to poison, but a backend that reached for one to compute a
 * waveform would be introducing exactly the kind of float hakoniwaos/ is
 * kept free of by hand, since there is no automated scanner for it (see
 * CLAUDE.md). No heap either: every implementation below either owns a
 * fixed buffer or streams samples on the fly.
 *
 * SILENCE IS THE SAFE DEFAULT. A backend with no speaker, no buzzer and no
 * amp wired -- which is every ESP32 build tonight, since Chris owns the
 * parts but has not soldered them yet -- returns KF_ERR_UNAVAILABLE and
 * makes no sound, never a crash, never a hang: the same "never a crash,
 * never a hang, never a silently wrong pet age" contract
 * ports/esp32/hal/esp_time.cpp documents for a missing DS3231, applied here
 * to a missing speaker. A caller that ignores the return value gets exactly
 * what it would have gotten anyway: quiet.
 *
 * Non-blocking. kf_audio_tone() starts the tone and returns immediately;
 * the tone plays out on its own over the next `ms` milliseconds, the same
 * "call it and it happens in the background" shape kf_display_present()'s
 * dirty rectangles already establish for a slow peripheral. A blocking
 * implementation would stall the frame loop for the tone's whole duration,
 * which on a 30fps budget (kf/budget.h) a 150ms chirp cannot afford.
 *
 * Valid C.
 */

#ifndef KF_HAL_AUDIO_H
#define KF_HAL_AUDIO_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_AUDIO_VERSION 1

/* The range a passive buzzer or a synthesised square wave can be trusted to
 * reproduce -- roughly a piano's bottom octave to just past the top of human
 * hearing. Not a claim about any specific buzzer's datasheet (Chris's five
 * modules are not on the bench to measure), just a sanity floor so
 * kf_audio_tone() can reject an obvious mistake (a script that meant
 * milliseconds and passed it as Hz) the same way kf/types.h's kf_button
 * bitmask rejects a name it does not recognise. */
#define KF_AUDIO_MIN_HZ 20u
#define KF_AUDIO_MAX_HZ 20000u

/* A ceiling on a single kf_audio_tone() call, not a musical opinion --
 * guards against a script bug (a stray extra zero) turning "chirp" into
 * "drone for a very long time" on a device with a speaker a few centimetres
 * from someone's ear. Five seconds is generous for anything this HAL is
 * meant for; a game that genuinely wants longer plays it as several calls. */
#define KF_AUDIO_MAX_MS 5000u

kf_result kf_audio_init(void);

/* Play a square-wave tone at `hz` for `ms` milliseconds, starting now.
 *
 * KF_ERR_INVALID if `hz` is outside [KF_AUDIO_MIN_HZ, KF_AUDIO_MAX_HZ], if
 * `ms` is 0, or if `ms` exceeds KF_AUDIO_MAX_MS -- these are caller mistakes,
 * not hardware conditions, so the caller finds out immediately rather than
 * getting silence and wondering why. hz==0 is deliberately not "silence": if
 * a caller wants silence, the correct call is not calling this at all (or
 * kf_audio_stop() to cut an existing tone short) -- a magic zero-frequency
 * value would be exactly the kind of not-obvious special case docs/sdk-
 * style-guide.md warns the layer above this one to avoid, and this HAL
 * holds itself to the same bar.
 *
 * KF_ERR_UNAVAILABLE on a backend with nothing to make sound with -- see
 * this header's own comment above on why that is the expected answer from
 * every ESP32 build until an amp or buzzer is actually soldered in. Still
 * KF_OK from the caller's point of view in the sense that nothing crashed:
 * check the return value only if silence itself is news.
 *
 * A tone already playing is replaced by this one, not queued behind it --
 * there is no tone queue. A caller that wants two tones in a row calls this
 * twice, `ms` apart, from its own frame loop; kf/hal/audio.h has no notion
 * of a sequence, matching kf/hal/power.h's own "this is not an
 * error-handling framework" restraint (kf/types.h's kf_result comment). */
kf_result kf_audio_tone(uint32_t hz, uint32_t ms);

/* Silence immediately, if a tone is currently sounding. A no-op, not an
 * error, if nothing is playing or the backend has nothing to make sound
 * with -- always safe to call, the same "never a crash, never a hang"
 * contract kf_audio_tone()'s KF_ERR_UNAVAILABLE path already promises. */
void kf_audio_stop(void);

void kf_audio_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_AUDIO_H */
