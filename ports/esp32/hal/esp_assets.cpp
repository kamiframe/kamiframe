/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: assets, ESP32 implementation.
 *
 * The real thing kf/hal/assets.h's own header comment describes: the
 * "assets" data partition (ports/esp32/partitions.csv) mapped directly into
 * the CPU's address space with esp_partition_mmap(), so sprite pixels are
 * read straight out of flash through the normal flash cache -- the same
 * path instruction fetches already use -- rather than copied into RAM
 * first. A 2MB PSRAM arena could not hold the flash asset budget even
 * once; this is what makes copying unnecessary rather than merely
 * undesirable.
 *
 * esp_partition_mmap() maps into ESP_PARTITION_MMAP_DATA space, which
 * (unlike ESP_PARTITION_MMAP_INST) allows ordinary byte-and-halfword-
 * aligned reads, not just 4-byte-aligned ones -- matching kf/assets.cpp's
 * own alignment requirement (pixel data is only guaranteed 4-byte aligned,
 * kf_color reads are 2-byte). Reading through the returned pointer is
 * ordinary memory access from the CPU's point of view: safe from any
 * context that can execute code from flash at all, which includes every
 * task and ISR on this chip apart from ones explicitly running with the
 * flash cache disabled (spi_flash's own erase/write critical sections,
 * none of which this file or the blitter ever runs inside). See
 * docs/architecture/adr-0033-asset-pipeline.md.
 */

#include "kf/hal/assets.h"

#include "kf/hal/log.h"

#include "esp_partition.h"

#include <cinttypes>

namespace {

constexpr const char *TAG = "assets";

const void *g_base = nullptr;
size_t g_size = 0u;
esp_partition_mmap_handle_t g_handle = 0u;
bool g_up = false;

} // namespace

kf_result kf_hal_assets_mount(const char *pack_path) {
    /* On-device there is no filesystem path to give -- there is exactly
     * one flash partition this can mean. The argument exists purely so
     * kf/hal/assets.h's signature is shared with the desktop backend. */
    (void)pack_path;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        KF_HAL_ASSETS_PARTITION_LABEL);
    if (part == nullptr) {
        KF_LOGE(TAG,
                "no partition named '%s' in the partition table -- see "
                "ports/esp32/partitions.csv",
                KF_HAL_ASSETS_PARTITION_LABEL);
        return KF_ERR_IO;
    }

    const void *ptr = nullptr;
    esp_partition_mmap_handle_t handle = 0u;
    const esp_err_t err = esp_partition_mmap(
        part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        KF_LOGE(TAG, "esp_partition_mmap('%s', %" PRIu32 " bytes) failed: %d",
                KF_HAL_ASSETS_PARTITION_LABEL, part->size, err);
        return KF_ERR_IO;
    }

    g_base = ptr;
    g_size = part->size;
    g_handle = handle;
    g_up = true;
    KF_LOGI(TAG, "asset partition '%s' mapped: %zu bytes at %p",
            KF_HAL_ASSETS_PARTITION_LABEL, g_size, ptr);
    return KF_OK;
}

const uint8_t *kf_hal_assets_base(void) {
    return g_up ? static_cast<const uint8_t *>(g_base) : nullptr;
}

size_t kf_hal_assets_size(void) { return g_up ? g_size : 0u; }

void kf_hal_assets_unmount(void) {
    if (g_up) {
        esp_partition_munmap(g_handle);
        g_base = nullptr;
        g_size = 0u;
        g_handle = 0u;
        g_up = false;
    }
}
