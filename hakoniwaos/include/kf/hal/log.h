/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * HAL: logging and panic.
 *
 * Small, but it is in the HAL for a reason: ESP_LOGx writes to a UART with a
 * particular format, desktop writes to stderr, and the headless CI backend
 * needs to capture output. More importantly, kf_panic means genuinely
 * different things: abort with a stack trace on desktop, versus put a legible
 * message on the panel and reboot on a device with no console attached.
 *
 * Valid C.
 */

#ifndef KF_HAL_LOG_H
#define KF_HAL_LOG_H

#include "kf/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KF_HAL_LOG_VERSION 1

typedef enum {
    KF_LOG_ERROR = 0,
    KF_LOG_WARN = 1,
    KF_LOG_INFO = 2,
    KF_LOG_DEBUG = 3
} kf_log_level;

/* printf-style. The format-attribute lets the compiler check the arguments,
 * which is worth having given how many of these will be written. */
#if defined(__GNUC__)
#define KF_PRINTFLIKE(fmt_index, first_arg)                                   \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define KF_PRINTFLIKE(fmt_index, first_arg)
#endif

void kf_log(kf_log_level level, const char *tag, const char *fmt, ...)
    KF_PRINTFLIKE(3, 4);

/* Unrecoverable. Does not return.
 *
 * This is what a blown arena, a failed static invariant or a missing backend
 * calls. It must be loud and it must stop: a constraint violation that prints
 * a warning and continues is a constraint that is not enforced. */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void kf_panic(const char *file, int line, const char *fmt, ...)
    KF_PRINTFLIKE(3, 4);

#define KF_PANIC(...) kf_panic(__FILE__, __LINE__, __VA_ARGS__)

/* Always compiled in, in every build configuration, on purpose. An invariant
 * that only holds in debug builds is not an invariant. If one of these is ever
 * hot enough to matter, that is a design problem, not a reason for NDEBUG. */
#define KF_ASSERT(cond, ...)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            kf_panic(__FILE__, __LINE__, __VA_ARGS__);                        \
        }                                                                     \
    } while (0)

#define KF_LOGE(tag, ...) kf_log(KF_LOG_ERROR, tag, __VA_ARGS__)
#define KF_LOGW(tag, ...) kf_log(KF_LOG_WARN, tag, __VA_ARGS__)
#define KF_LOGI(tag, ...) kf_log(KF_LOG_INFO, tag, __VA_ARGS__)
#define KF_LOGD(tag, ...) kf_log(KF_LOG_DEBUG, tag, __VA_ARGS__)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KF_HAL_LOG_H */
