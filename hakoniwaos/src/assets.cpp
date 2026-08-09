/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Parses the .kfpack format tools/kf_pack_assets.py writes (see that file's
 * own header comment for the exact byte layout) and builds a small,
 * type-tagged table in KF_ARENA_ASSETS, each row pointing straight into the
 * bytes kf/hal/assets.h handed back -- never copied. See kf/assets.h for
 * the full reasoning on both of those choices, and on why the directory
 * walk below validates every entry the same way regardless of its
 * kf_asset_type and only branches on type for the last, type-specific
 * decode step.
 *
 * Every multi-byte field is read with explicit little-endian byte-at-a-time
 * unpacking (read_u16/read_u32 below), not a struct overlay: the mapped
 * base pointer's alignment is only guaranteed to whatever the backend
 * promises (word-aligned in practice on both, but this file does not need
 * to assume that to stay correct), and reading byte-by-byte sidesteps
 * strict-aliasing and alignment questions entirely for a few dozen fields
 * read once at startup, where the cost genuinely does not matter.
 */

#include "kf/assets.h"

#include "kf/arena.h"
#include "kf/budget.h"
#include "kf/hal/assets.h"
#include "kf/hal/log.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "kf/poison.h"

namespace {

constexpr const char *TAG = "assets";

constexpr uint8_t kMagic[4] = {'K', 'F', 'A', 'P'};
constexpr uint16_t kFormatVersion = 1;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kEntryBytes = 52;
constexpr size_t kNameBytes = 32;
constexpr size_t kTypeMetaBytes = 8;

/* One table row per directory entry, of ANY kf_asset_type -- the directory
 * walk in kf_assets_init() below builds one of these for every entry it
 * validates, not just sprites. `sprite` is only meaningful when `type ==
 * KF_ASSET_TYPE_SPRITE`; for any other type it is left zeroed, which is
 * harmless because kf_assets_get() checks `type` before ever handing it
 * back. A future kf_assets_get_clip() would do the equivalent: scan this
 * same table, check `type == KF_ASSET_TYPE_AUDIO_CLIP`, and decode
 * `data_offset`/`data_bytes`/the raw `type_meta` bytes into whatever a PCM
 * clip view needs -- an addition here, not a second table or a second
 * directory walk. */
struct AssetEntry {
    char name[kNameBytes];
    kf_asset_type type;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint8_t type_meta[kTypeMetaBytes]; /* raw, type-specific; see
                                         * tools/kf_pack_assets.py's format
                                         * comment for what each type puts
                                         * here */
    kf_sprite sprite; /* decoded from type_meta iff type == SPRITE */
};

AssetEntry *g_entries = nullptr;
int g_entry_count = 0;
bool g_up = false;

uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                  (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

const AssetEntry *find_entry(const char *name) {
    if (!g_up || name == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < g_entry_count; ++i) {
        if (std::strcmp(g_entries[i].name, name) == 0) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

} // namespace

kf_result kf_assets_init(void) {
    KF_ASSERT(!g_up, "kf_assets_init called twice");

    const kf_result mounted = kf_hal_assets_mount(nullptr);
    KF_ASSERT(mounted == KF_OK,
              "asset pack could not be mounted (see the log above) -- "
              "kf_assets_init has no degraded mode for a missing pack");

    const uint8_t *base = kf_hal_assets_base();
    const size_t size = kf_hal_assets_size();
    KF_ASSERT(base != nullptr && size >= kHeaderBytes,
              "asset pack is missing or too small to hold even a header "
              "(%zu bytes, need at least %zu)",
              size, kHeaderBytes);
    /* The one budget check that belongs to core rather than either
     * backend: KF_FLASH_ASSET_BUDGET_BYTES is what ports/esp32/
     * partitions.csv's "assets" partition is sized to, and this is what
     * makes exceeding it a hard failure on desktop too, not a limit only
     * the device happens to enforce. See kf/budget.h's own header banner. */
    KF_ASSERT(size <= KF_FLASH_ASSET_BUDGET_BYTES,
              "asset pack is %zu bytes, over KF_FLASH_ASSET_BUDGET_BYTES "
              "(%zu) in kf/budget.h -- it will not fit the flash partition "
              "either; regenerate a smaller pack or raise the budget "
              "deliberately, in its own commit, per that file's own rule",
              size, static_cast<size_t>(KF_FLASH_ASSET_BUDGET_BYTES));

    KF_ASSERT(std::memcmp(base, kMagic, sizeof(kMagic)) == 0,
              "asset pack does not start with the KFAP magic -- wrong "
              "file, or corrupt");
    const uint16_t version = read_u16(base + 4);
    KF_ASSERT(version == kFormatVersion,
              "asset pack is format version %u, this build reads version "
              "%u -- regenerate it with tools/kf_pack_assets.py",
              version, kFormatVersion);
    const uint16_t entry_count = read_u16(base + 6);
    const uint32_t directory_offset = read_u32(base + 8);

    KF_ASSERT(entry_count <= KF_ASSETS_MAX_ENTRIES,
              "asset pack has %u entries, more than KF_ASSETS_MAX_ENTRIES "
              "(%d) in kf/assets.h -- raise that bound deliberately if a "
              "pack this size is genuinely needed",
              entry_count, KF_ASSETS_MAX_ENTRIES);

    const size_t directory_bytes =
        static_cast<size_t>(entry_count) * kEntryBytes;
    KF_ASSERT(directory_offset <= size &&
                  directory_bytes <= size - directory_offset,
              "asset pack directory (%u entries at offset %" PRIu32
              ") runs past the end of the file (%zu bytes) -- corrupt pack",
              entry_count, directory_offset, size);

    g_entries = static_cast<AssetEntry *>(kf_arena_alloc(
        KF_ARENA_ASSETS,
        sizeof(AssetEntry) *
            static_cast<size_t>(entry_count == 0u ? 1u : entry_count),
        alignof(AssetEntry)));
    g_entry_count = 0;

    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint8_t *e =
            base + directory_offset + static_cast<size_t>(i) * kEntryBytes;
        AssetEntry &row = g_entries[g_entry_count];
        std::memcpy(row.name, e, kNameBytes);
        /* Defend against a name field that used all 32 bytes with no NUL
         * at all -- see tools/kf_pack_assets.py's format comment. */
        row.name[kNameBytes - 1] = '\0';

        /* Generic fields, validated identically for every entry regardless
         * of type -- this IS the "type-agnostic directory walk" kf/
         * assets.h's own header comment describes. Layout: name(32)
         * asset_type(1) reserved(1) reserved(2) type_meta(8)
         * data_offset(4) data_bytes(4) = 52 bytes, see
         * tools/kf_pack_assets.py. */
        const uint8_t asset_type_raw = e[32];
        std::memcpy(row.type_meta, e + 36, kTypeMetaBytes);
        const uint32_t data_offset = read_u32(e + 44);
        const uint32_t data_bytes = read_u32(e + 48);

        row.type = static_cast<kf_asset_type>(asset_type_raw);
        row.data_offset = data_offset;
        row.data_bytes = data_bytes;
        row.sprite = kf_sprite{};

        KF_ASSERT(data_offset <= size && data_bytes <= size - data_offset,
                  "asset '%s': data (offset %" PRIu32 ", %" PRIu32
                  " bytes) runs past the end of the file (%zu bytes) -- "
                  "corrupt pack",
                  row.name, data_offset, data_bytes, size);
        /* Every payload is addressed straight into the mounted pack with
         * no copy, so it must be at least 2-byte aligned for a kf_color*
         * (or, later, an int16_t PCM sample*) cast over it to be defined;
         * the packer always 4-aligns it (see its own "Alignment"
         * comment), this just refuses to trust a pack that did not. */
        KF_ASSERT(data_offset % 4u == 0u,
                  "asset '%s': data at offset %" PRIu32 " is not 4-byte "
                  "aligned -- corrupt pack, or a packer that stopped "
                  "aligning entries",
                  row.name, data_offset);

        /* The only place kf_asset_type is branched on: everything above
         * this line already validated and stored the entry regardless of
         * what type it turns out to be. Unrecognised types (today, that
         * is anything other than SPRITE -- AUDIO_CLIP has no decoder yet)
         * simply get no type-specific view built; the row still exists in
         * the table, ready for a future decoder to read the same
         * data_offset/data_bytes/type_meta this one leaves in place. */
        if (row.type == KF_ASSET_TYPE_SPRITE) {
            const uint16_t width = read_u16(row.type_meta + 0);
            const uint16_t height = read_u16(row.type_meta + 2);
            const uint16_t color_key = read_u16(row.type_meta + 4);
            const bool has_color_key = row.type_meta[6] != 0u;

            const uint64_t expected_bytes = static_cast<uint64_t>(width) *
                                             static_cast<uint64_t>(height) *
                                             sizeof(kf_color);
            KF_ASSERT(data_bytes == expected_bytes,
                      "sprite '%s': data_bytes (%" PRIu32
                      ") does not match width*height*2 (%llu) -- corrupt "
                      "pack",
                      row.name, data_bytes,
                      static_cast<unsigned long long>(expected_bytes));

            row.sprite.pixels =
                reinterpret_cast<const kf_color *>(base + data_offset);
            row.sprite.width = width;
            row.sprite.height = height;
            row.sprite.color_key = color_key;
            row.sprite.has_color_key = has_color_key;
        }

        g_entry_count++;
    }

    g_up = true;
    KF_LOGI(TAG, "asset pack mounted: %d entr%s, %zu bytes", g_entry_count,
            g_entry_count == 1 ? "y" : "ies", size);
    return KF_OK;
}

const kf_sprite *kf_assets_get(const char *name) {
    const AssetEntry *e = find_entry(name);
    if (e == nullptr || e->type != KF_ASSET_TYPE_SPRITE) {
        return nullptr;
    }
    return &e->sprite;
}

void kf_assets_shutdown(void) {
    if (!g_up) {
        return;
    }
    kf_hal_assets_unmount();
    g_entries = nullptr;
    g_entry_count = 0;
    g_up = false;
}
