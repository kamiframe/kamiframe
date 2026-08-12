/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Simulator-private inspection hooks for the headless backend. Not HAL, not
 * visible to core.
 */

#ifndef KF_HEADLESS_PROBE_H
#define KF_HEADLESS_PROBE_H

#include <cstdint>

/* FNV-1a over every framebuffer presented so far. Deterministic for a given
 * seed and frame count, which is what makes it usable as a CI assertion. */
uint64_t kf_headless_checksum(void);

uint64_t kf_headless_frames(void);

/* Total pixels reported as dirty across the run. Guards against a change that
 * quietly starts redrawing the whole screen every frame: that would still
 * look correct, and would still be a regression, because on the device it is
 * the difference between 30fps and 60. */
uint64_t kf_headless_dirty_pixels(void);

/* Scripted button input, so CI can drive the app deterministically. Returns
 * the mask for the given frame. */
uint32_t kf_headless_script(uint64_t frame);

/* headless_audio.cpp's recording of what kf_audio_tone()/kf_audio_stop()
 * were asked to do, so "did it make the right sound at the right moment" is
 * a real assertion instead of a hope -- see that file's own header comment.
 * Counters accumulate for the whole process lifetime, the same as kf_
 * headless_frames()/kf_headless_dirty_pixels() above; every check in this
 * binary runs as its own process (one ctest entry each), so there is no
 * cross-check contamination to reset between. */

/* How many times kf_audio_tone() was called with IN-RANGE arguments and
 * actually started a tone -- a call rejected as KF_ERR_INVALID does not
 * count, matching kf_audio_tone()'s own contract that an invalid call makes
 * no sound. */
uint64_t kf_headless_audio_tone_count(void);

/* The `hz`/`ms` of the MOST RECENT accepted kf_audio_tone() call. Only
 * meaningful when kf_headless_audio_tone_count() is nonzero -- both read 0
 * before the first accepted call, the same "meaningless before its own
 * readiness flag" convention kf.hour()/kf.minute() document in sdk/lua/
 * kf_lua_port.cpp. */
uint32_t kf_headless_audio_last_hz(void);
uint32_t kf_headless_audio_last_ms(void);

/* How many times kf_audio_stop() was called, regardless of whether a tone
 * was actually sounding at the time -- kf_audio_stop() is documented as
 * always safe to call, so this backend counts every call, not just the ones
 * that silenced something. */
uint64_t kf_headless_audio_stop_count(void);

/* Zeroes every counter above. UNLIKE kf_headless_checksum()/kf_headless_
 * frames()/kf_headless_dirty_pixels(), which accumulate for a whole ctest
 * process because every check that cares about them runs as its own
 * process, run_audio_check() (headless_main.cpp) exercises several
 * self-contained sections back to back INSIDE ONE process invocation --
 * direct HAL calls, then the Lua binding, then the real interactive app --
 * and each section's assertions are written in terms of "exactly N tones
 * since this section started", not "since the process started". Called
 * once between each such section. */
void kf_headless_audio_reset(void);

#endif /* KF_HEADLESS_PROBE_H */
