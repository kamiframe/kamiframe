/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * See kf_dbg_codec.h for what these do and why they have zero ESP-IDF
 * dependency. Every function here is a plain loop over a caller-owned
 * buffer -- no allocation, no I/O -- so it behaves identically whether it
 * is linked into the firmware or into the throwaway host test program in
 * this same directory's ADR "Verified" section.
 */

#include "kf_dbg_codec.h"

namespace {

constexpr char kBase64Alphabet[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Standard reflected CRC-32 table, built once at first use rather than
 * hand-transcribed: transcription is exactly the kind of subtly-wrong
 * mistake this whole file exists to avoid (see kf_dbg_bridge.h's own
 * comment on why RLE/base64 got a host test before device trust). Built
 * from the bit-at-a-time definition, which is easy to read and to check
 * against the polynomial's own documentation. */
struct Crc32Table {
    uint32_t entries[256];

    constexpr Crc32Table() : entries{} {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; bit++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            entries[i] = c;
        }
    }
};

constexpr Crc32Table kCrc32Table;

} // namespace

size_t kf_dbg_rle_encode(const uint16_t *pixels, size_t pixel_count,
                          uint8_t *out, size_t out_cap) {
    size_t out_len = 0;
    size_t i = 0;
    while (i < pixel_count) {
        const uint16_t px = pixels[i];
        size_t run = 1;
        /* Cap at 65535 (0xFFFF): the wire format's count field is exactly
         * 16 bits. Matches tools/kf_debug_selftest.py's own reference
         * encoder's `run < 0xFFFF` cap. */
        while (i + run < pixel_count && pixels[i + run] == px &&
               run < 0xFFFFu) {
            run++;
        }

        if (out_len + 4 > out_cap) {
            return static_cast<size_t>(-1);
        }
        const uint16_t count16 = static_cast<uint16_t>(run);
        out[out_len++] = static_cast<uint8_t>(count16 & 0xFFu);
        out[out_len++] = static_cast<uint8_t>((count16 >> 8) & 0xFFu);
        out[out_len++] = static_cast<uint8_t>(px & 0xFFu);
        out[out_len++] = static_cast<uint8_t>((px >> 8) & 0xFFu);

        i += run;
    }
    return out_len;
}

size_t kf_dbg_base64_encoded_len(size_t len) {
    return ((len + 2) / 3) * 4;
}

size_t kf_dbg_base64_encode(const uint8_t *data, size_t len, char *out,
                             size_t out_cap) {
    const size_t needed = kf_dbg_base64_encoded_len(len);
    if (out_cap < needed) {
        return static_cast<size_t>(-1);
    }

    size_t oi = 0;
    size_t i = 0;
    /* Three input bytes -> four output characters, the ordinary case. */
    for (; i + 3 <= len; i += 3) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                            (static_cast<uint32_t>(data[i + 1]) << 8) |
                            static_cast<uint32_t>(data[i + 2]);
        out[oi++] = kBase64Alphabet[(n >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(n >> 12) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(n >> 6) & 0x3Fu];
        out[oi++] = kBase64Alphabet[n & 0x3Fu];
    }

    /* 1 or 2 leftover bytes: pad with '=' per RFC 4648. */
    const size_t remaining = len - i;
    if (remaining == 1) {
        const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out[oi++] = kBase64Alphabet[(n >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(n >> 12) & 0x3Fu];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (remaining == 2) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                            (static_cast<uint32_t>(data[i + 1]) << 8);
        out[oi++] = kBase64Alphabet[(n >> 18) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(n >> 12) & 0x3Fu];
        out[oi++] = kBase64Alphabet[(n >> 6) & 0x3Fu];
        out[oi++] = '=';
    }

    return oi;
}

uint32_t kf_dbg_crc32_init(void) {
    return 0xFFFFFFFFu;
}

uint32_t kf_dbg_crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; i++) {
        crc = kCrc32Table.entries[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t kf_dbg_crc32_final(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

uint32_t kf_dbg_crc32(const void *data, size_t len) {
    return kf_dbg_crc32_final(kf_dbg_crc32_update(kf_dbg_crc32_init(), data, len));
}
