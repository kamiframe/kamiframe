/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL backend: logging and panic, host implementation.
 *
 * kf_panic aborts rather than exits, so a debugger stops on the frame that
 * caused it and the core dump is useful. On the device this will instead put
 * the message on the panel and reboot, because there is no console attached
 * to a virtual pet.
 */

#include "kf/hal/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

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
    std::fprintf(stderr, "[%s] %-6s ", level_name(level), tag ? tag : "-");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

void kf_panic(const char *file, int line, const char *fmt, ...) {
    std::fflush(stdout);
    std::fprintf(stderr,
                 "\n"
                 "================================================\n"
                 " HakoniwaOS panic\n"
                 " %s:%d\n"
                 "------------------------------------------------\n",
                 file, line);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr,
                 "\n"
                 "================================================\n"
                 "\n"
                 "This is a hard stop on purpose. A constraint that warns\n"
                 "and carries on is not a constraint.\n");
    std::fflush(stderr);
    std::abort();
}
