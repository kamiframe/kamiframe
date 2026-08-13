/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: audio, ESP32 implementation.
 *
 * NOTHING IN THIS FILE HAS BEEN RUN ON A SPEAKER since the queue redesign
 * below -- the buzzer/amp bring-up caveats this file's header comment has
 * carried since the sound foundation task still apply in full: I2S has no
 * acknowledgement the way I2C does, so a clean compile and a clean init log
 * prove the driver initialised, never that a tone reached a speaker.
 *
 * kf_esp_pins.h's own comment on KF_ESP_PIN_I2S_BCLK/WS/DOUT/DIN calls them
 * "reserved, never wired" -- that is still true after this file. Chris owns
 * a MAX98357A I2S amp (x2) and a passive buzzer (x5), but the buzzer is not
 * given a pin at all (kf_esp_pins.h's own reasoning: "redundant with the
 * amplifier"), and the amp has not been soldered to BCLK(GPIO1)/WS(GPIO2)/
 * DOUT(GPIO9) yet. This driver targets exactly those three pins in I2S
 * standard master/TX-only mode, generating the square wave kf/hal/audio.h
 * documents in software and streaming it out over I2S -- the same waveform
 * a passive buzzer would produce directly from a toggled GPIO, just carried
 * over a different wire. When the amp is soldered in, this should work
 * as-is; if it does not, that is real information a real board will
 * produce, not something to guess at from a desk.
 *
 * ================================================================
 * THE QUEUE: WHOLE SEQUENCES, DEPTH 1 -- NOT INDIVIDUAL NOTES, DEPTH N.
 * ================================================================
 *
 * The bug this fixes: every sound in tools/kf_chiptune.py's SOUNDS table is
 * multi-note, but the ORIGINAL version of this file queued one (hz, ms)
 * message per kf_audio_tone() call, depth 1, xQueueOverwrite(). Two
 * kf_audio_tone() calls close together did not play two notes in order --
 * the second call's xQueueOverwrite() replaced the first message before
 * audio_task() ever woke up to play it, so only the LAST note of any
 * multi-note phrase ever reached the speaker. A creature.lua that called
 * kf.tone() four times in a row to spell out "C7 C7 C7 F7" would have
 * played a single F7 on real hardware and nothing else.
 *
 * Two shapes were available to fix this:
 *
 *   (a) Deepen the queue so several NOTE messages can queue up, one per
 *       kf_audio_tone() call, and let audio_task() drain them in order.
 *   (b) Make the unit that gets queued a whole SEQUENCE (kf_audio_note[],
 *       count, duty) instead of one note, and keep the queue at depth 1.
 *
 * (b) is what this file does, and the reason is a failure mode (a) has that
 * (b) does not: a creature whose want goes unanswered keeps re-pinging
 * (examples/creature_demo/creature.lua's own level-crossing want-ping
 * system -- three pings per unmet need, at threshold crossings). Under (a),
 * every one of those calls enqueues its own few notes, and if the device is
 * slower to drain the queue than the pings arrive (or the player is simply
 * away from the desk for the whole escalation), the notes pile up: a
 * creature ignored for twenty minutes would eventually blurt out a BACKLOG
 * of nine or twelve stale chirps back to back the next time anyone looked
 * at it, which is a worse experience than the bug being fixed, not a
 * better one. Under (b), a new call to kf_audio_play_notes() -- via
 * xQueueOverwrite() on this same depth-1 queue, exactly as kf_audio_tone()
 * always has -- REPLACES whatever sequence was queued or mid-playback, so
 * the creature only ever plays the single most current thing it has to
 * say, matching kf/hal/audio.h's own documented "replaces, not queued
 * behind it" contract extended to a whole phrase. Within one call, the
 * notes themselves DO play in order (the bug above is still fixed) -- what
 * changed is that the unit of "replaces, not queued" grew from one note to
 * one phrase, not that queuing behaviour was added where there was none.
 *
 * WHAT THIS MEANS FOR THE STUCK-TONE FIX (the other constraint this file's
 * comments already carried, and the reason a naive "just deepen the queue"
 * change would have been actively wrong): the channel-enable/disable dance
 * documented on audio_task() below -- enable, stream, silence-flush,
 * disable, in that order -- now happens ONCE PER SEQUENCE, not once per
 * note. Enabling and disabling between every note of a 7-note phrase
 * (hatch/evolve, the longest in the approved set) would mean 7x the
 * enable/disable calls, 7x the silence-flush "audible spit" risk the
 * original fix's own comment describes, and -- worse -- 7 separate windows
 * where a mid-phrase kf_audio_stop() could race the disable the way the
 * original bug did. One enable at the start of the sequence and one
 * disable at the end, with the stop flag polled between (and within) notes
 * so kf_audio_stop() still interrupts promptly, keeps that fix's own
 * invariant intact: THE CHANNEL IS ENABLED PER PLAYBACK AND DISABLED AGAIN
 * BEFORE THE NEXT ONE STARTS, where "playback" now means a whole sequence.
 *
 * NON-BLOCKING, per kf/hal/audio.h's contract: kf_audio_tone()/kf_audio_
 * tone_duty()/kf_audio_play_notes() only validate their arguments and post
 * one message to a queue, then return -- a dedicated FreeRTOS task (created
 * once, here, at init) does the actual blocking i2s_channel_write() calls
 * that stream the waveform out. Without this, any of the three would stall
 * the caller's frame for the whole sequence's duration, which app.cpp's
 * 30fps budget (kf/budget.h) cannot absorb for even a single 150ms chirp,
 * let alone a 580ms hatch fanfare.
 */

#include "kf/hal/audio.h"

#include "kf/hal/log.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kf_esp_pins.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char *TAG = "audio";

/* Same rate the desktop SDL backend uses (simulator/src/sdl/sdl_audio.cpp)
 * -- not load-bearing that the two match (nothing compares them), just
 * convenient for anyone reading both files side by side. Comfortably above
 * 2x KF_AUDIO_MAX_HZ (20kHz), so this sample rate can represent every
 * frequency this HAL accepts. */
constexpr uint32_t kSampleRateHz = 44100;

/* Silence between consecutive notes of one sequence, matching tools/
 * kf_chiptune.py's own render() `gap_ms=8` default and simulator/src/sdl/
 * sdl_audio.cpp's identical constant -- so a phrase played here sounds like
 * the same phrase heard in that preview tool or on Chris's Mac, not a
 * backend-specific approximation of it. Not applied after the LAST note of
 * a sequence -- the silence-flush already below handles that. */
constexpr uint32_t kNoteGapMs = 8;

/* One SEQUENCE message -- see this file's own header comment for why the
 * unit that gets queued is a whole sequence, not one note. Depth-1 queue,
 * xQueueOverwrite() -- kf_audio_tone()/_tone_duty()/kf_audio_play_notes()
 * all funnel through kf_audio_play_notes() below, which posts exactly one
 * of these and always REPLACES whatever was queued or mid-playback,
 * matching kf/hal/audio.h's own documented contract. */
struct SequenceMsg {
    uint32_t hz[KF_AUDIO_MAX_NOTES];
    uint32_t ms[KF_AUDIO_MAX_NOTES];
    uint32_t count;
    uint32_t duty_permille;
    uint32_t gain_permille; /* kf_audio_set_volume()'s effect, snapshotted
                              * at enqueue time -- see kf_audio_play_notes()
                              * below for why a snapshot, not a live read,
                              * is the right choice here. */
};

i2s_chan_handle_t g_tx = nullptr;
QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
bool g_ready = false;

/* KF_VOLUME_4 -- see kf_audio_get_volume()'s own header comment (kf/hal/
 * audio.h) for why that, not KF_VOLUME_OFF, is the process-start default. */
kf_volume_level g_volume = KF_VOLUME_4;

/* Set by kf_audio_stop(), polled by audio_task() between write chunks --
 * a plain flag rather than a queue message, since it needs to interrupt
 * whichever sequence is already mid-playback, not wait behind it. */
std::atomic<bool> g_stop_requested{false};

/* KF_VOLUME_1..4 map onto 1/4, 2/4, 3/4, 4/4 of full amplitude -- matches
 * simulator/src/sdl/sdl_audio.cpp's identical table exactly (see that
 * file's own comment on why an even split, not a tuned curve). KF_VOLUME_
 * OFF has no entry: kf_audio_play_notes() below skips enqueuing at all
 * when muted, so this table never sees a 0. */
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

/* How many samples make one full square-wave period at `hz`, at this
 * file's fixed sample rate. Integer division, same reasoning sdl_audio.cpp
 * gives for staying float-free even outside hakoniwaos/ core: there is no
 * reason for a backend to reach for a float where integer period math
 * already gives the exact same waveform a buzzer's own GPIO toggle would.
 * Returns 0 if `hz` is high enough that even one sample-rate period rounds
 * away to nothing -- cannot happen with KF_AUDIO_MAX_HZ (20kHz) against a
 * 44.1kHz sample rate, kept as a guard rather than an assert because it
 * costs nothing. */
uint32_t period_samples(uint32_t hz) { return kSampleRateHz / hz; }

/* Streams one note (a rest, hz == 0, is silence) in fixed-size chunks,
 * checking g_stop_requested between chunks so kf_audio_stop() -- or a NEW
 * sequence replacing this one -- can interrupt promptly. Returns false if
 * it was stopped before finishing (the caller uses this to skip the rest
 * of the sequence and the inter-note gap, going straight to the silence-
 * flush). Does NOT enable/disable the I2S channel itself -- see this
 * file's own header comment for why that happens once per SEQUENCE, in the
 * caller, not once per note here. */
bool stream_note(uint32_t hz, uint32_t ms, uint32_t duty_permille,
                  uint32_t gain_permille) {
    constexpr size_t kChunkSamples = 256;
    static int16_t chunk[kChunkSamples];

    const uint64_t total_samples =
        (static_cast<uint64_t>(kSampleRateHz) * ms) / 1000ull;
    const uint32_t period = hz == 0u ? 0u : period_samples(hz);
    uint32_t on_samples = 0;
    if (period > 0u) {
        on_samples = (period * duty_permille) / 1000u;
        if (on_samples == 0u) {
            on_samples = 1u;
        } else if (on_samples >= period) {
            on_samples = period - 1u;
        }
    }
    const int16_t peak = static_cast<int16_t>(
        (static_cast<int32_t>(8000) * static_cast<int32_t>(gain_permille)) /
        1000); /* 8000: headroom below INT16_MAX, matching this file's
                 * original single-tone amplitude exactly */

    uint64_t played = 0;
    uint64_t phase = 0; /* sample index within the current waveform, carried
                          * across chunks so a chunk boundary never
                          * introduces a click */
    while (played < total_samples &&
           !g_stop_requested.load(std::memory_order_relaxed)) {
        const uint64_t remaining = total_samples - played;
        const size_t this_chunk = remaining < kChunkSamples
                                       ? static_cast<size_t>(remaining)
                                       : kChunkSamples;
        if (period == 0u) {
            std::memset(chunk, 0, this_chunk * sizeof(int16_t)); /* a rest */
        } else {
            for (size_t i = 0; i < this_chunk; ++i) {
                const bool high = ((phase + i) % period) < on_samples;
                chunk[i] = high ? peak : static_cast<int16_t>(-peak);
            }
        }
        size_t written = 0;
        /* 100ms timeout: long enough that a healthy DMA ring never
         * legitimately hits it, short enough that a wedged bus does not
         * hang this task for more than a tenth of a second. */
        const esp_err_t err = i2s_channel_write(
            g_tx, chunk, this_chunk * sizeof(int16_t), &written, 100);
        if (err != ESP_OK) {
            KF_LOGW(TAG, "i2s_channel_write failed: %s",
                    esp_err_to_name(err));
            return false;
        }
        phase += this_chunk;
        played += this_chunk;
    }
    return played >= total_samples;
}

/* The dedicated audio task: blocks on the queue, then streams a WHOLE
 * sequence out over I2S -- every note in order, each followed by a short
 * silence gap -- until either every note has played or kf_audio_stop() (or
 * a replacing kf_audio_play_notes() call) sets g_stop_requested. */
void audio_task(void * /*arg*/) {
    for (;;) {
        SequenceMsg msg{};
        if (xQueueReceive(g_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        g_stop_requested.store(false, std::memory_order_relaxed);

        /* THE CHANNEL IS ENABLED PER SEQUENCE AND DISABLED AGAIN BELOW, and
         * that is the whole fix for the stuck-tone bug this cost us on real
         * hardware -- see this file's own header comment for the full
         * account (Chris, 2026-08-12: "as soon as the chirp started, it
         * would not stop the sound... I had to unplug it") and for why this
         * enable/disable now brackets a whole sequence rather than each
         * note within it. An enabled I2S TX channel does NOT go quiet when
         * you stop feeding it -- its DMA ring keeps cycling, so an underrun
         * replays whatever bytes were last in those buffers. Silence, then
         * disable, in that order: disabling alone genuinely does stop the
         * output, but it leaves a buffer full of loud samples for the NEXT
         * enable to start replaying, which is an audible spit at the front
         * of every sequence. */
        if (i2s_channel_enable(g_tx) != ESP_OK) {
            KF_LOGW(TAG, "i2s_channel_enable failed -- skipping this sequence");
            continue;
        }

        const uint32_t gap_samples =
            (kSampleRateHz * kNoteGapMs) / 1000u;
        static int16_t kSilenceChunk[512] = {};

        for (uint32_t i = 0;
             i < msg.count && !g_stop_requested.load(std::memory_order_relaxed);
             ++i) {
            const bool finished_note = stream_note(
                msg.hz[i], msg.ms[i], msg.duty_permille, msg.gain_permille);
            if (!finished_note) {
                break; /* stopped mid-note -- go straight to the flush below */
            }
            /* The inter-note gap, not applied after the LAST note (nothing
             * to separate it from -- the silence-flush already covers
             * that). Interruptible the same way a note's own chunks are. */
            if (i + 1u < msg.count) {
                uint32_t remaining_gap = gap_samples;
                while (remaining_gap > 0u &&
                       !g_stop_requested.load(std::memory_order_relaxed)) {
                    const size_t this_chunk =
                        remaining_gap < (sizeof(kSilenceChunk) / sizeof(int16_t))
                            ? remaining_gap
                            : (sizeof(kSilenceChunk) / sizeof(int16_t));
                    size_t written = 0;
                    if (i2s_channel_write(g_tx, kSilenceChunk,
                                           this_chunk * sizeof(int16_t),
                                           &written, 100) != ESP_OK) {
                        break;
                    }
                    remaining_gap -= static_cast<uint32_t>(this_chunk);
                }
            }
        }

        /* Flush the DMA ring with silence, then stop the peripheral --
         * ONCE for the whole sequence, matching this file's own header
         * comment on why the enable above also happens once. Sized
         * generously against I2S_CHANNEL_DEFAULT_CONFIG's ring (6
         * descriptors x 240 frames = 1440 on IDF v5+, and this does not
         * hard-code that number so a future IDF changing it cannot quietly
         * under-flush). Best-effort throughout: every path below ends in a
         * disable regardless, because a failed flush must still not leave
         * a tone running -- that is the failure being fixed. */
        for (int i = 0; i < 16; ++i) {
            size_t written = 0;
            if (i2s_channel_write(g_tx, kSilenceChunk, sizeof kSilenceChunk,
                                   &written, 20) != ESP_OK) {
                break;
            }
        }
        if (i2s_channel_disable(g_tx) != ESP_OK) {
            /* Loud, because the audible symptom is a stuck tone and the
             * only remaining recovery is a power cycle. */
            KF_LOGE(TAG, "i2s_channel_disable failed -- a tone may be stuck");
        }
    }
}

} // namespace

kf_result kf_audio_init(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &g_tx, nullptr) != ESP_OK) {
        KF_LOGW(TAG, "i2s_new_channel failed -- audio disabled");
        return KF_OK; /* never a crash, never a hang -- see this file's own
                        * header comment */
    }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = KF_ESP_PIN_I2S_BCLK;
    std_cfg.gpio_cfg.ws = KF_ESP_PIN_I2S_WS;
    std_cfg.gpio_cfg.dout = KF_ESP_PIN_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED; /* TX only -- the microphone
                                              * (INMP441, shares BCLK/WS with
                                              * this amp per kf_esp_pins.h)
                                              * is a separate, not-yet-built
                                              * RX-side driver */

    if (i2s_channel_init_std_mode(g_tx, &std_cfg) != ESP_OK) {
        KF_LOGW(TAG, "i2s_channel_init_std_mode failed -- audio disabled");
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return KF_OK;
    }
    /* DELIBERATELY NOT ENABLED HERE. The channel is enabled per SEQUENCE by
     * audio_task() and disabled again as soon as that sequence ends -- see
     * this file's own header comment. Enabling at init is what made the
     * first chirp on real hardware play forever: an idle-but-enabled TX
     * channel replays its DMA ring rather than going quiet. Leaving it
     * disabled between sequences is also the only state in which "no
     * sound" is guaranteed.
     *
     * Consequence for anything added later: i2s_channel_enable() is not
     * idempotent (it returns ESP_ERR_INVALID_STATE on an already-enabled
     * channel), so a second enable path would need to track state rather
     * than call it blindly. */

    g_queue = xQueueCreate(1, sizeof(SequenceMsg));
    if (g_queue == nullptr) {
        KF_LOGW(TAG, "xQueueCreate failed -- audio disabled");
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return KF_OK;
    }

    if (xTaskCreate(audio_task, "kf_audio", 3072, nullptr,
                     tskIDLE_PRIORITY + 1, &g_task) != pdPASS) {
        KF_LOGW(TAG, "xTaskCreate failed -- audio disabled");
        vQueueDelete(g_queue);
        g_queue = nullptr;
        /* No disable here: nothing enabled the channel. Init stopped doing
         * that when the per-sequence enable landed -- see the comment
         * above. */
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return KF_OK;
    }

    g_ready = true;
    KF_LOGI(TAG, "I2S TX on BCLK=%d WS=%d DOUT=%d, %lu Hz -- UNVERIFIED, "
                 "nothing soldered to these pins as of this build",
            static_cast<int>(KF_ESP_PIN_I2S_BCLK),
            static_cast<int>(KF_ESP_PIN_I2S_WS),
            static_cast<int>(KF_ESP_PIN_I2S_DOUT),
            static_cast<unsigned long>(kSampleRateHz));
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
    if (!g_ready) {
        return KF_ERR_UNAVAILABLE;
    }
    if (g_volume == KF_VOLUME_OFF) {
        /* GENUINELY silent: nothing is enqueued at all -- see kf_audio_set_
         * volume()'s own header comment (kf/hal/audio.h) for why "off"
         * means this, not a zero-amplitude sequence sent to the task
         * anyway. Also matches kf_audio_tone()'s existing "replaces, not
         * queued behind it" contract in spirit: a muted request does not
         * even take the place of whatever silent state the channel is
         * already in. */
        return KF_OK;
    }

    /* `gain_permille` is snapshotted from g_volume HERE, at enqueue time,
     * not read live by audio_task() while streaming -- a volume change
     * mid-sequence (kf_audio_set_volume() also calls kf_audio_stop(), so
     * this is a narrow window) should not retroactively change the gain of
     * a sequence already queued; the NEXT kf_audio_play_notes() call picks
     * up the new volume, exactly the same "settings take effect on the
     * next thing said, not by rewriting what is already mid-sentence"
     * behaviour the queue-replacement design elsewhere in this file
     * already has. */
    SequenceMsg msg{};
    for (uint32_t i = 0; i < count; ++i) {
        msg.hz[i] = notes[i].hz;
        msg.ms[i] = notes[i].ms;
    }
    msg.count = count;
    msg.duty_permille = duty_permille;
    msg.gain_permille = volume_gain_permille(g_volume);

    /* Interrupt whatever is currently playing (kf_audio_play_notes()'s own
     * "replaces, not queued behind it" contract), THEN overwrite the queue
     * -- xQueueOverwrite() on a depth-1 queue always leaves exactly this
     * request for audio_task() to pick up next, whether or not a previous
     * one was still sitting there unread. */
    g_stop_requested.store(true, std::memory_order_relaxed);
    xQueueOverwrite(g_queue, &msg);
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
    g_stop_requested.store(true, std::memory_order_relaxed);
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
    if (g_task != nullptr) {
        /* Deleting the task can land mid-sequence, with the channel enabled
         * and a loud DMA ring -- exactly the stuck-tone state. The disable
         * below is therefore load-bearing on this path, not tidy-up, and
         * must stay after the delete: doing it first would let the task
         * write again before it dies. Its return value is ignored on
         * purpose, since the channel may legitimately already be disabled
         * (task idle between sequences), and "already off" is the outcome
         * wanted either way. */
        vTaskDelete(g_task);
        g_task = nullptr;
    }
    if (g_queue != nullptr) {
        vQueueDelete(g_queue);
        g_queue = nullptr;
    }
    if (g_tx != nullptr) {
        (void)i2s_channel_disable(g_tx);
        i2s_del_channel(g_tx);
        g_tx = nullptr;
    }
    g_ready = false;
}
