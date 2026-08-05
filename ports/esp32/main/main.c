/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * ESP-IDF entry point. See ADR 0019 for why this is deliberately a plain
 * ESP-IDF hello-world and not a call into hakoniwaos's app loop.
 *
 * This is Phase 1's second named hardware exit trigger, per README.md:
 * "an ESP-IDF hello-world boots in Wokwi." It exists to prove the build
 * system shape works, nothing more. `kf_app_init()` / `kf_app_frame()`
 * (kf/app.h) need real ESP32 HAL backends -- ST7789 over SPI, GPIO input,
 * esp_timer/RTC, heap_caps_malloc pools -- none of which exist yet (see
 * ../README.md's "What still has to be written"). Calling them here would
 * either fail to link or silently run against stub HAL functions that were
 * never verified against real hardware, which is worse than not calling
 * them at all.
 *
 * What this file DOES prove, by existing and building at all: `hakoniwaos`
 * is REQUIRES'd by the `main` component (see CMakeLists.txt in this
 * directory), which pulls every file in hakoniwaos/sources.cmake through
 * the real xtensa-esp32s3 cross-compiler as part of this build. That's the
 * architectural claim ADR 0002 and this port's README make -- one source
 * list, two build systems -- actually exercised, not just documented.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void) {
    printf("kamiframe: hello from ESP-IDF\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("kamiframe: chip=%s cores=%d revision=%d\n",
           CONFIG_IDF_TARGET, chip_info.cores, chip_info.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("kamiframe: flash=%" PRIu32 "MB\n", flash_size / (1024u * 1024u));
    }

    printf("kamiframe: free heap=%" PRIu32 " bytes\n", esp_get_free_heap_size());

    /* A real hakoniwaos source file, compiled for this target moments ago
     * as part of this same build. Not called from here (see the header
     * comment above for why), but its presence in the component graph is
     * the point: this is proof the shared source list cross-compiles. */
    printf("kamiframe: hakoniwaos component linked, HAL backends pending "
           "(Phase 1b)\n");

    int seconds = 0;
    while (1) {
        printf("kamiframe: alive, %ds\n", seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ++seconds;
    }
}
