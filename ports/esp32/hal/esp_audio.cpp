/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: audio, ESP32 implementation.
 *
 * NOTHING IN THIS FILE HAS BEEN RUN. This is bring-up discipline, the same
 * as ADR 0020 asked of esp_power.cpp before there was a board at all:
 * implement the contract correctly against the datasheet and the driver
 * docs, document exactly what is unverified, and stop there rather than
 * pretend a compile is a proof.
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
 * WHAT HAPPENS WITH NOTHING SOLDERED: i2s_new_channel()/i2s_channel_init_
 * std_mode()/i2s_channel_enable() all succeed even with nothing connected
 * to BCLK/WS/DOUT -- I2S is a clocked serial bus with no acknowledgement,
 * unlike the DS3231's I2C in esp_time.cpp, so there is no "nothing answered"
 * failure path to detect here. A board with no amp wired will run this
 * driver's whole init sequence successfully and then silently produce no
 * sound at all, because there is nothing on the other end of the wire to
 * turn samples into air pressure. That is the correct, safe behaviour --
 * matches this HAL's own "silence is the safe default" contract even though
 * the reason is "nobody would hear it" rather than "the driver declined to
 * start" -- but it is worth stating plainly rather than leaving a reader to
 * assume a clean KF_OK here means audio was actually verified to reach a
 * speaker. IT WAS NOT.
 *
 * NON-BLOCKING, per kf/hal/audio.h's contract: kf_audio_tone() only
 * validates its arguments and posts a tiny message to a queue, then returns
 * -- a dedicated FreeRTOS task (created once, here, at init) does the actual
 * blocking i2s_channel_write() calls that stream the waveform out. Without
 * this, kf_audio_tone() would stall the caller's frame for the whole tone
 * duration, which app.cpp's 30fps budget (kf/budget.h) cannot absorb for
 * even a 150ms chirp.
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

namespace {

constexpr const char *TAG = "audio";

/* Same rate the desktop SDL backend uses (simulator/src/sdl/sdl_audio.cpp)
 * -- not load-bearing that the two match (nothing compares them), just
 * convenient for anyone reading both files side by side. Comfortably above
 * 2x KF_AUDIO_MAX_HZ (20kHz), so this sample rate can represent every
 * frequency this HAL accepts. */
constexpr uint32_t kSampleRateHz = 44100;

/* One in-flight tone message at a time -- kf_audio_tone() replaces rather
 * than queues (kf/hal/audio.h's own documented contract), so depth 1 is
 * correct, not merely "small enough": xQueueOverwrite() below always keeps
 * exactly the most recent request. */
struct ToneMsg {
    uint32_t hz;
    uint32_t ms;
};

i2s_chan_handle_t g_tx = nullptr;
QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
bool g_ready = false;

/* Set by kf_audio_stop(), polled by kf_audio_task() between write chunks --
 * a plain flag rather than a queue message, since it needs to interrupt
 * whichever tone is already mid-playback, not wait behind it. */
std::atomic<bool> g_stop_requested{false};

/* How many samples make one square-wave half-cycle at `hz`, at this file's
 * fixed sample rate. Integer division, same reasoning sdl_audio.cpp gives
 * for staying float-free even outside hakoniwaos/ core: there is no reason
 * for a backend to reach for a float where integer period math already
 * gives the exact same waveform a buzzer's own GPIO toggle would. Returns 0
 * if `hz` is high enough that even one sample rate period rounds away to
 * nothing -- cannot happen with KF_AUDIO_MAX_HZ (20kHz) against a 44.1kHz
 * sample rate (a little over 2 samples per half-cycle at worst), kept as a
 * guard rather than an assert because it costs nothing. */
uint32_t half_period_samples(uint32_t hz) {
    return (kSampleRateHz / hz) / 2u;
}

/* The dedicated audio task: blocks on the queue, then streams a square wave
 * out over I2S in fixed-size chunks until either the requested duration has
 * played or kf_audio_stop() sets g_stop_requested. A static (not stack, not
 * heap) chunk buffer -- this is a backend file under ports/esp32/, not
 * hakoniwaos/ core, so tools/check_no_heap.py does not scan it and using
 * the heap here would be fine, but a fixed buffer this small (512 bytes)
 * costs nothing to keep static and removes one more thing that could fail
 * at runtime on a device with 8MB of PSRAM to spare elsewhere. */
void audio_task(void * /*arg*/) {
    constexpr size_t kChunkSamples = 256;
    static int16_t chunk[kChunkSamples];
    constexpr int16_t kAmplitude = 8000; /* headroom below INT16_MAX */

    for (;;) {
        ToneMsg msg{};
        if (xQueueReceive(g_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        g_stop_requested.store(false, std::memory_order_relaxed);

        const uint32_t half_period = half_period_samples(msg.hz);
        if (half_period == 0u) {
            continue; /* cannot happen within this HAL's own Hz ceiling */
        }
        const uint64_t total_samples =
            (static_cast<uint64_t>(kSampleRateHz) * msg.ms) / 1000ull;

        uint64_t played = 0;
        uint64_t phase = 0; /* sample index within the current waveform,
                              * carried across chunks so a chunk boundary
                              * never introduces a click */
        while (played < total_samples &&
               !g_stop_requested.load(std::memory_order_relaxed)) {
            const uint64_t remaining = total_samples - played;
            const size_t this_chunk = remaining < kChunkSamples
                                           ? static_cast<size_t>(remaining)
                                           : kChunkSamples;
            for (size_t i = 0; i < this_chunk; ++i) {
                const bool high = ((phase + i) / half_period) % 2u == 0u;
                chunk[i] = high ? kAmplitude : static_cast<int16_t>(-kAmplitude);
            }
            size_t written = 0;
            /* 100ms timeout: long enough that a healthy DMA ring never
             * legitimately hits it, short enough that a wedged bus does not
             * hang this task (and therefore never hangs kf_audio_stop() /
             * kf_audio_shutdown(), which do not wait on this task at all --
             * see those functions below) for more than a tenth of a
             * second. */
            const esp_err_t err = i2s_channel_write(
                g_tx, chunk, this_chunk * sizeof(int16_t), &written, 100);
            if (err != ESP_OK) {
                KF_LOGW(TAG, "i2s_channel_write failed: %s",
                        esp_err_to_name(err));
                break;
            }
            phase += this_chunk;
            played += this_chunk;
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
    if (i2s_channel_enable(g_tx) != ESP_OK) {
        KF_LOGW(TAG, "i2s_channel_enable failed -- audio disabled");
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return KF_OK;
    }

    g_queue = xQueueCreate(1, sizeof(ToneMsg));
    if (g_queue == nullptr) {
        KF_LOGW(TAG, "xQueueCreate failed -- audio disabled");
        i2s_channel_disable(g_tx);
        i2s_del_channel(g_tx);
        g_tx = nullptr;
        return KF_OK;
    }

    if (xTaskCreate(audio_task, "kf_audio", 3072, nullptr,
                     tskIDLE_PRIORITY + 1, &g_task) != pdPASS) {
        KF_LOGW(TAG, "xTaskCreate failed -- audio disabled");
        vQueueDelete(g_queue);
        g_queue = nullptr;
        i2s_channel_disable(g_tx);
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

kf_result kf_audio_tone(uint32_t hz, uint32_t ms) {
    if (hz < KF_AUDIO_MIN_HZ || hz > KF_AUDIO_MAX_HZ) {
        return KF_ERR_INVALID;
    }
    if (ms == 0u || ms > KF_AUDIO_MAX_MS) {
        return KF_ERR_INVALID;
    }
    if (!g_ready) {
        return KF_ERR_UNAVAILABLE;
    }

    /* Interrupt whatever is currently playing (kf_audio_tone()'s own
     * "replaces, not queued behind" contract), THEN overwrite the queue --
     * xQueueOverwrite() on a depth-1 queue always leaves exactly this
     * request for audio_task() to pick up next, whether or not a previous
     * one was still sitting there unread. */
    g_stop_requested.store(true, std::memory_order_relaxed);
    const ToneMsg msg{hz, ms};
    xQueueOverwrite(g_queue, &msg);
    return KF_OK;
}

void kf_audio_stop(void) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void kf_audio_shutdown(void) {
    if (g_task != nullptr) {
        vTaskDelete(g_task);
        g_task = nullptr;
    }
    if (g_queue != nullptr) {
        vQueueDelete(g_queue);
        g_queue = nullptr;
    }
    if (g_tx != nullptr) {
        i2s_channel_disable(g_tx);
        i2s_del_channel(g_tx);
        g_tx = nullptr;
    }
    g_ready = false;
}
