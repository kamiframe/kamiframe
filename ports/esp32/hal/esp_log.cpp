/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: logging and panic, ESP32 implementation.
 *
 * kf_log routes to ESP-IDF's own esp_log, so every line still gets IDF's
 * usual timestamp/tag prefix on the UART console -- the same console
 * `idf.py monitor` (or Wokwi's serial monitor, see ADR 0019) already reads.
 *
 * kf_panic does NOT paint a message on the panel. kf/hal/log.h's own header
 * comment names that as the eventual device behaviour ("a legible message
 * on the panel and reboot"), but doing that safely means the panic path
 * must work even if the crash happened before kf_display_init() ran, or
 * mid-frame with the SPI bus in an unknown state -- a real, separate piece
 * of engineering this HAL-scaffolding slice (ADR 0020) is not scoped to
 * do. What this DOES do: log the exact same loud banner the desktop build
 * prints (host_log.cpp), flush the UART, then esp_restart() -- a clean
 * reboot rather than desktop's abort()/core dump, because there is no
 * debugger attached to a virtual pet and a device that hangs after a panic
 * is worse than one that comes back up. Painting the panel is real future
 * work, not a silently dropped feature.
 */

#include "kf/hal/log.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#include <cstdarg>
#include <cstdio>

namespace {

/* One shared tag for everything this HAL backend logs through -- the
 * caller's own `tag` argument still appears in the message body, same as
 * the desktop backend's "[%s] %-6s " prefix, just formatted as ESP-IDF's
 * own esp_log wants it: ESP_LOGx takes ONE compile-time tag, not a runtime
 * one, so kf_log's runtime `tag` is folded into the format string instead
 * of trying to fight that. */
constexpr const char *kEspLogTag = "kf";

const char *level_name(kf_log_level level) {
    switch (level) {
    case KF_LOG_ERROR:
        return "E";
    case KF_LOG_WARN:
        return "W";
    case KF_LOG_INFO:
        return "I";
    case KF_LOG_DEBUG:
        return "D";
    }
    return "?";
}

} // namespace

void kf_log(kf_log_level level, const char *tag, const char *fmt, ...) {
    char message[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    /* esp_log's own ESP_LOGx macros pick the level via a compile-time
     * constant per call site, which kf_log's runtime `level` argument
     * can't use directly -- esp_log_write() is the underlying function
     * every ESP_LOGx macro expands to, and it takes the level as a normal
     * argument, so it is the correct thing to call here, not a workaround. */
    esp_log_level_t esp_level;
    switch (level) {
    case KF_LOG_ERROR:
        esp_level = ESP_LOG_ERROR;
        break;
    case KF_LOG_WARN:
        esp_level = ESP_LOG_WARN;
        break;
    case KF_LOG_INFO:
        esp_level = ESP_LOG_INFO;
        break;
    case KF_LOG_DEBUG:
    default:
        esp_level = ESP_LOG_DEBUG;
        break;
    }

    esp_log_write(esp_level, kEspLogTag, "[%s] %-6s %s\n", level_name(level),
                  tag ? tag : "-", message);
}

void kf_panic(const char *file, int line, const char *fmt, ...) {
    char message[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    esp_log_write(ESP_LOG_ERROR, kEspLogTag,
                  "\n"
                  "================================================\n"
                  " HakoniwaOS panic\n"
                  " %s:%d\n"
                  "------------------------------------------------\n"
                  "%s\n"
                  "================================================\n"
                  "\n"
                  "This is a hard stop on purpose. A constraint that warns\n"
                  "and carries on is not a constraint. Rebooting.\n",
                  file, line, message);

    /* No display-panic path yet (see this file's header comment) -- give
     * the UART a moment to actually get the message out before the reset
     * tears everything down. esp_rom_delay_us() is a ROM busy-wait, not a
     * scheduler call, which matters here: a panic can happen before
     * FreeRTOS is fully up or from a context where blocking on the
     * scheduler would be wrong, so this deliberately does not use
     * vTaskDelay() the way kf_time_delay_us() does. (First choice here was
     * a hand-rolled `for (volatile int i = ...; ++i)` loop -- the real
     * xtensa-esp32s3 compiler rejected it outright: incrementing a volatile
     * int is deprecated as of C++26/-Werror=volatile, which this project
     * builds with. esp_rom_delay_us() is both the fix and the more correct
     * tool for the job.) */
    esp_rom_delay_us(20000);
    esp_restart();
}
