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

/* Bumped to 2 for the sound-foundation follow-up: duty-cycle control
 * (kf_audio_tone_duty()) and a sequence-aware primitive (kf_audio_play_
 * notes()) -- both additive, kf_audio_tone()'s own signature and contract
 * are unchanged. */
#define KF_HAL_AUDIO_VERSION 2

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
 * A tone already playing is replaced by this one, not queued behind it. See
 * kf_audio_play_notes() below for the one thing that changed this: a
 * caller that genuinely wants several notes IN ORDER should call that, not
 * hand-roll a sequence out of repeated kf_audio_tone() calls `ms` apart from
 * its own frame loop -- on the ESP32 backend specifically, each such call
 * still replaces whatever the previous one queued, so a hand-rolled
 * sequence called faster than the device can finish playing loses notes,
 * exactly the bug the sound foundation's second task fixed by making the
 * SEQUENCE, not the individual note, the unit that gets queued. */
kf_result kf_audio_tone(uint32_t hz, uint32_t ms);

/* kf_audio_tone(), plus a duty cycle: the fraction of each cycle the
 * square wave spends "high", in permille (parts per thousand) of a full
 * period -- KF_AUDIO_DUTY_THIN/_MID/_FAT below name the three tiers this
 * project actually uses, matching tools/kf_chiptune.py's own THIN/MID/FAT
 * constants exactly so a phrase tuned in that preview tool sounds the same
 * on a real backend. Same oscillator as kf_audio_tone(), no extra cost --
 * only the compare against `phase` changes, not the waveform generation
 * itself. kf_audio_tone(hz, ms) is exactly kf_audio_tone_duty(hz, ms,
 * KF_AUDIO_DUTY_FAT) -- the two share one underlying implementation per
 * backend, they are not independently maintained.
 *
 * KF_ERR_INVALID if `duty_permille` falls outside
 * [KF_AUDIO_DUTY_MIN, KF_AUDIO_DUTY_MAX], for the same reason kf_audio_
 * tone()'s own hz/ms range checks exist: an obviously wrong value (0, or
 * 1000) is a caller mistake worth surfacing immediately rather than a
 * degenerate always-low or always-high "square" wave that is not really a
 * tone any more. */
kf_result kf_audio_tone_duty(uint32_t hz, uint32_t ms, uint32_t duty_permille);

/* Named duty tiers, permille (0..1000) of one square-wave period spent
 * "high". Mirror tools/kf_chiptune.py's THIN/MID/FAT exactly -- see that
 * file's own header comment for the design reasoning (12.5% reads as the
 * creature's own thin, nasal voice; 50% is the fat classic beep used for
 * system fanfares; 25% sits between the two for the ordinary "you did
 * something" care-response jingles). Kept as three named constants rather
 * than raw numbers at every call site, the same reasoning KF_AUDIO_MIN_HZ
 * gets a name instead of a bare 20. */
#define KF_AUDIO_DUTY_THIN 125u  /* an eighth (125/1000): the creature's own
                                   * voice */
#define KF_AUDIO_DUTY_MID 250u   /* 25%: ordinary care-response jingles */
#define KF_AUDIO_DUTY_FAT 500u   /* 50%: system fanfares; kf_audio_tone()'s
                                   * own duty */
#define KF_AUDIO_DUTY_MIN 10u    /* 1% -- below this the wave is close enough
                                   * to silent-with-clicks to not be a tone */
#define KF_AUDIO_DUTY_MAX 990u   /* 99% -- the mirror image of the floor
                                   * above */

/* One note in a kf_audio_play_notes() sequence. `hz` of 0 is a REST --
 * unlike kf_audio_tone()'s own hz, which treats 0 as a caller mistake (see
 * that function's own comment), a rest is a legitimate, expected token in a
 * note sequence: tools/kf_chiptune.py's own note spec grammar has one
 * ("-"/"r"/"R"), and "want_rest"'s own spec ("E6:70 -:30 B6:120") uses one
 * for exactly the pause that makes the phrase read as unhurried rather than
 * a straight run of notes. `ms` is the note's duration in milliseconds, the
 * same unit and the same non-zero requirement as kf_audio_tone()'s own
 * `ms`. */
typedef struct {
    uint32_t hz;
    uint32_t ms;
} kf_audio_note;

/* A ceiling on how many notes one kf_audio_play_notes() call can hold, not
 * a musical opinion -- every sound in tools/kf_chiptune.py's SOUNDS table
 * (the approved set) tops out at 7 notes (hatch/evolve, the motif's fullest
 * extension); this is more than double that for headroom, the same
 * "generous, not tuned to the current content" reasoning KF_AUDIO_MAX_MS
 * gets. Backends hold a sequence in a fixed-size buffer sized by this
 * constant -- no heap, matching this header's own no-heap rule. */
#define KF_AUDIO_MAX_NOTES 16u

/* A ceiling on the SUM of a sequence's own note durations (rests included,
 * gaps a backend inserts between notes for playback quality are not), the
 * sequence-level equivalent of KF_AUDIO_MAX_MS's per-tone ceiling and for
 * the identical reason: a script bug building a very long note list should
 * not turn "a chirp" into "a drone", on a device with a speaker centimetres
 * from someone's ear. */
#define KF_AUDIO_MAX_SEQUENCE_MS 5000u

/* Play `count` notes, in order, at `duty_permille`, starting now --
 * kf.melody() (sdk/lua/kf_lua_port.cpp) is the SDK surface built directly on
 * this. This is the sequence-aware primitive kf_audio_tone() itself is
 * built from (one note, KF_AUDIO_DUTY_FAT) -- see this header's own comment
 * on kf_audio_tone() for why a caller that wants several notes in a row
 * should reach for THIS rather than several kf_audio_tone() calls.
 *
 * REPLACES, NOT QUEUED BEHIND -- exactly kf_audio_tone()'s own contract,
 * extended to a whole sequence rather than one note: calling this while a
 * previous sequence is still sounding INTERRUPTS it. This is a deliberate
 * choice, not the only one available -- a queue of individual notes across
 * separate calls was the other real option, and was rejected because it has
 * a genuine failure mode this one does not: a creature whose want goes
 * unanswered keeps re-pinging as its need crosses further thresholds (see
 * examples/creature_demo/creature.lua's own level-crossing want pings), and
 * a per-note queue would let those pile up behind whatever was already
 * playing, so an ignored pet on a desk would eventually blurt out a backlog
 * of stale chirps in a burst instead of the single most current one.
 * Replacing keeps exactly one thing true at any moment: the sound now
 * playing is always the most recent thing this creature had to say. See
 * ports/esp32/hal/esp_audio.cpp's own header comment for the implementation
 * this reasoning drove (a depth-1 queue of whole sequences, not a deeper
 * queue of individual notes).
 *
 * KF_ERR_INVALID if `notes` is NULL, `count` is 0 or exceeds KF_AUDIO_MAX_
 * NOTES, any note's `ms` is 0, any note's non-zero `hz` falls outside
 * [KF_AUDIO_MIN_HZ, KF_AUDIO_MAX_HZ], the sum of every note's `ms` exceeds
 * KF_AUDIO_MAX_SEQUENCE_MS, or `duty_permille` falls outside [KF_AUDIO_
 * DUTY_MIN, KF_AUDIO_DUTY_MAX] -- validated before anything is enqueued or
 * touches hardware, the same "the caller finds out immediately" reasoning
 * kf_audio_tone()'s own KF_ERR_INVALID path documents.
 *
 * KF_ERR_UNAVAILABLE on a backend with nothing to make sound with, same as
 * kf_audio_tone(). */
kf_result kf_audio_play_notes(const kf_audio_note *notes, uint32_t count,
                               uint32_t duty_permille);

/* Silence immediately, if a tone or a note sequence is currently sounding.
 * A no-op, not an error, if nothing is playing or the backend has nothing
 * to make sound with -- always safe to call, the same "never a crash, never
 * a hang" contract kf_audio_tone()'s KF_ERR_UNAVAILABLE path already
 * promises. */
void kf_audio_stop(void);

/* Five positions: OFF, then four increasing loudness levels. Owner's own
 * words: "add a volume setting ... with 4 levels of sound. The 5th level
 * would be 'off' completely." Ordered so KF_VOLUME_OFF is the falsy
 * default (matching kf_pet_want's own "0 is NONE" convention, kf/pet.h) and
 * the rest increase, so "louder" reads naturally as "a bigger number". */
typedef enum {
    KF_VOLUME_OFF = 0,
    KF_VOLUME_1 = 1,
    KF_VOLUME_2 = 2,
    KF_VOLUME_3 = 3,
    KF_VOLUME_4 = 4,

    /* NOT A VOLUME. A range guard, and the reason it exists is subtle
     * enough to be worth the clutter.
     *
     * Without it, this enum's valid range is derived from its enumerators
     * and stops at 7 (the smallest bit-field that holds 4). Casting
     * anything larger into the type is then UNSPECIFIED behaviour, not
     * merely an out-of-range value -- and out-of-range values genuinely
     * occur here, because the level is persisted and restored, and
     * kf_audio_set_volume() clamps precisely because it does not trust what
     * it is handed. A clamp against a value the language says is
     * meaningless is not a clamp.
     *
     * GCC says so out loud under -Wconversion ("the result of the
     * conversion is unspecified because '99' is outside the range of type
     * kf_volume_level"). It was invisible locally because CI builds with
     * -DKAMIFRAME_WARNINGS_AS_ERRORS=ON and an ordinary local build does
     * not.
     *
     * A fixed underlying type (`enum : uint8_t`) would be the tidier fix
     * and is deliberately NOT used: that is C23, this project builds its
     * HAL headers as C17, and ADR 0001 commits them to staying
     * C-compatible. This spelling works in both languages.
     *
     * KF_VOLUME_LEVEL_COUNT below stays 5 -- anything iterating real levels
     * must use that, never this. Any switch over kf_volume_level needs a
     * default case, which every one in the tree already has. */
    KF_VOLUME_LEVEL_RANGE_GUARD = 255,
} kf_volume_level;

#define KF_VOLUME_LEVEL_COUNT 5u

/* Sets the amplitude every subsequent kf_audio_tone()/kf_audio_tone_duty()/
 * kf_audio_play_notes() call is scaled by, until the next call to this
 * function. A DEVICE setting, not a per-call one -- there is deliberately
 * no volume argument on any of the three calls above, the same "the
 * Settings screen calls this; your game almost certainly should not" line
 * kf.set_clock()'s own header comment (sdk/lua/kf_lua_port.cpp) draws for
 * the wall clock.
 *
 * KF_VOLUME_OFF means GENUINELY silent: every backend below skips actually
 * producing or queuing output at all rather than playing a zero-amplitude
 * waveform, so "off" is provably silent (nothing reaches the speaker, the
 * DMA ring, or SDL's stream), not merely quiet enough not to notice. Also
 * silences whatever is currently sounding (kf_audio_stop()'s own effect),
 * so a volume change -- OFF or otherwise -- takes effect immediately, not
 * only on the next sound.
 *
 * Always succeeds: there is no hardware condition to fail against, only an
 * internal scale factor. An out-of-range `level` clamps to the nearest
 * valid one rather than being treated as an error -- the one caller this
 * project has (kf_lua_port_apply_volume(), applying a value read back from
 * kf/settings.h's persisted byte) could in principle hand this a corrupted
 * value, and a clamped, defined volume is a safer failure than refusing to
 * set any volume at all, the same reasoning kf_pet_reaction_to()'s own
 * out-of-range handling gives (kf/pet.h). */
void kf_audio_set_volume(kf_volume_level level);

/* The level most recently set by kf_audio_set_volume(), or KF_VOLUME_4
 * (this header's own "closest to the pre-existing always-full-volume
 * behaviour" default) if that has never been called this process. */
kf_volume_level kf_audio_get_volume(void);

void kf_audio_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_AUDIO_H */
