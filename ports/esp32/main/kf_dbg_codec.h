/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Pure, portable encoders for the KFDBG serial debug bridge (see
 * kf_dbg_bridge.h and docs/architecture/adr-0030-serial-debug-bridge.md):
 * run-length encoding for the framebuffer, base64 for putting arbitrary
 * bytes on an ASCII-only console, and CRC32 for the host to detect a
 * corrupted transfer.
 *
 * Deliberately free of every ESP-IDF/FreeRTOS header -- no
 * <driver/uart.h>, no <freertos/FreeRTOS.h>, nothing. That is what let
 * this be compiled and exercised as an ordinary host program before a
 * single byte of it ran against real hardware (see the ADR's "Verified"
 * section for the exact host-side test run). kf_dbg_bridge.cpp is where
 * the ESP-IDF-specific wiring -- tasks, queues, the UART itself -- lives.
 *
 * Matched against tools/kf_debug.py and tools/kf_debug_selftest.py, the
 * host side of this same bridge, already written and passing its own
 * selftest: base64 alphabet (RFC 4648, '=' padded), 76-char wrap width,
 * and CRC32 algorithm (zlib.crc32, i.e. IEEE 802.3) all match that file
 * byte for byte, not just in spirit.
 */

#ifndef KF_DBG_CODEC_H
#define KF_DBG_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RLE-encodes `pixel_count` RGB565 pixels from `pixels` into `out` as a
 * stream of (uint16 count, uint16 pixel) pairs, both little-endian --
 * exactly the wire format the protocol spec fixes for what an `fb` reply
 * decodes (post-base64) to. One pair per run of identical consecutive
 * pixels; a run longer than 65535 pixels (the field's own width) is split
 * into consecutive pairs for the same pixel value rather than ever writing
 * a wider count -- matches kf_debug_selftest.py's own reference encoder's
 * `run < 0xFFFF` cap exactly.
 *
 * Returns the number of bytes written, or (size_t)-1 if `out_cap` is too
 * small to hold the encoding. A caller that sizes `out_cap` to
 * `pixel_count * 4` -- the true worst case, one length-1 run per pixel,
 * which is what a screen of pure noise would produce -- never sees that
 * return value; every real UI screen compresses far smaller. */
size_t kf_dbg_rle_encode(const uint16_t *pixels, size_t pixel_count,
                          uint8_t *out, size_t out_cap);

/* Exact size of the base64 (RFC 4648, standard alphabet, '=' padded, no
 * line breaks of its own) encoding of `len` bytes: ceil(len/3)*4. */
size_t kf_dbg_base64_encoded_len(size_t len);

/* Standard base64. Writes exactly kf_dbg_base64_encoded_len(len) characters
 * to `out` and returns that count, or returns (size_t)-1 without writing
 * anything if `out_cap` is smaller than that. Does not NUL-terminate --
 * callers that want a C string add one byte of headroom and do it
 * themselves. */
size_t kf_dbg_base64_encode(const uint8_t *data, size_t len, char *out,
                             size_t out_cap);

/* Standard CRC-32: the IEEE 802.3 / zlib polynomial (0xEDB88320,
 * reflected), init 0xFFFFFFFF, final XOR 0xFFFFFFFF -- the exact algorithm
 * Python's `zlib.crc32` (what tools/kf_debug.py calls on the decoded
 * payload) and every other zip/PNG/Ethernet implementation uses, chosen
 * for that ubiquity: the parallel host-side task needed to write or import
 * nothing bespoke, and didn't.
 *
 * kf_dbg_crc32() is the one-shot convenience form. The three-call
 * init/update/final form lets a caller CRC several pieces back to back as
 * if they were one contiguous buffer, without first concatenating them
 * into one. kf_dbg_crc32(data, len) == kf_dbg_crc32_final(kf_dbg_crc32_
 * update(kf_dbg_crc32_init(), data, len)) for every input -- the codec
 * test program checks exactly that equivalence. */
uint32_t kf_dbg_crc32(const void *data, size_t len);
uint32_t kf_dbg_crc32_init(void);
uint32_t kf_dbg_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t kf_dbg_crc32_final(uint32_t crc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_DBG_CODEC_H */
