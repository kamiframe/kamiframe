/* SPDX-License-Identifier: Apache-2.0
 * Copyright the Kamiframe contributors.
 *
 * Makes the heap a compile error inside core.
 *
 * ============================================================================
 *  HOW TO USE THIS: include it LAST, after every system header, in each
 *  hakoniwaos/src/ source file. Never include it from a header, and never from a
 *  HAL backend (backends legitimately allocate; that is their job).
 * ============================================================================
 *
 * Core does not use the heap. It uses fixed arenas from kf/arena.h whose sizes
 * come from kf/budget.h. That is the whole constraint story, so an accidental
 * malloc is not a style problem, it is a hole in the enforcement.
 *
 * #pragma GCC poison works at the token level: after this point the identifier
 * simply cannot appear, and there is no way to switch it off from the command
 * line. It is supported by GCC and Clang, and the ESP32 toolchain is GCC, so
 * it holds where it matters most.
 *
 * MSVC ignores it. That is the main reason the CI matrix has a Linux GCC job:
 * on Windows this file is decorative, on Linux it has teeth.
 *
 * Two known limits, stated honestly:
 *   - It cannot see global operator new / delete, because those are not
 *     identifiers. tools/check_no_heap.py greps for those and runs in CI.
 *   - Including <cstdlib> (or anything that pulls it in) after this header
 *     will fail to compile, because the declaration of malloc mentions the
 *     poisoned token. That is a feature: core should not need <cstdlib>.
 */

#ifndef KF_POISON_H
#define KF_POISON_H

#if defined(__GNUC__) && !defined(KF_ALLOW_HEAP)
#pragma GCC poison malloc calloc realloc free strdup aligned_alloc
#endif

#endif /* KF_POISON_H */
