/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ASSERT_H
#define MAZU_ASSERT_H

#include <mazu/asm.h>
#include <mazu/base.h>
#include <mazu/print.h>
#include <mazu/stringdef.h>

#define crash(msg) __crash(msg, __FILE__, __LINE__)

#define __crash(msg, file, line)                                \
    do {                                                        \
        print_str(STR(file ":" STRINGIFY(line) ": " msg "\n")); \
        halt_execution();                                       \
    } while (0)

#define assert(x) __assert((x), #x, __FILE__, __LINE__)

#define __assert(x, x_str, file, line)                           \
    do {                                                         \
        if (!(x))                                                \
            __crash("Assertion '" x_str "' failed", file, line); \
    } while (0)

/* Two-tier assertion macros (ref: brittle-kernel k/panic.h).
 *
 * ALWAYS_ASSERT - production invariants.  Always compiled, even in optimized
 *   builds.  Use for conditions whose violation indicates a fundamental logic
 *   error or memory corruption - silent continuation is worse than halt.
 *
 * DEBUG_ASSERT - development consistency checks.  Compiled out when
 *   __DEBUG__ == 0 (the default non-debug build).  Use for redundant
 *   validation that catches bugs during development but has measurable
 *   performance cost in production.
 */
#define ALWAYS_ASSERT(cond) __assert((cond), #cond, __FILE__, __LINE__)

#if __DEBUG__ > 0
#define DEBUG_ASSERT(cond) ALWAYS_ASSERT(cond)
#else
#define DEBUG_ASSERT(cond) ((void) 0)
#endif

/* Magic-number validation macro for kernel structures (Q2).
 * Checks in debug builds, compiles to nothing otherwise.
 */
#define MAGIC_CHECK(obj, expected)                                         \
    do {                                                                   \
        if (__DEBUG__ > 0 && (obj)->magic != (expected)) {                 \
            print_str(STR(__FILE__ ":" TOSTRING(__LINE__) ": magic check " \
                                                          "failed\n"));    \
            halt_execution();                                              \
        }                                                                  \
    } while (0)

#endif /* MAZU_ASSERT_H */
