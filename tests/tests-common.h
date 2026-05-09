/* SPDX-License-Identifier: MIT */
/* Common test helpers for Mazu selftests.
 *
 * Included by individual test files to reduce boilerplate. Provides:
 * - SELFTEST_ASSERT: return-on-fail with contextual error code
 * - SMP task coordination helpers (require mazu/ipi.h and mazu/sched.h
 *   from the including source file)
 */

#ifndef TESTS_COMMON_H
#define TESTS_COMMON_H

#include <mazu/selftest.h>

/* Return 'rc' from the enclosing function if 'cond' is false.
 * Keeps the lightweight selftest convention (return nonzero = fail)
 * while giving each assertion site a unique error code for debugging.
 *
 * Usage:
 *   SELFTEST_ASSERT(ptr != NULL, 1);
 *   SELFTEST_ASSERT(count == 5, 2);
 */
#define SELFTEST_ASSERT(cond, rc) \
    do {                          \
        if (!(cond))              \
            return (rc);          \
    } while (0)

/* SMP task coordination helpers.  These depend on ipi_send_broadcast()
 * and sleep_ms() being declared by the including translation unit.
 * Guarded by MAZU_IPI_H so non-SMP test files (bcache, sfs) can
 * safely include this header without pulling in scheduler internals.
 */
#ifdef MAZU_IPI_H

/* Wait for a volatile bool to become true, kicking secondary harts
 * out of WFI between iterations.  Returns 1 (timeout) or 0 (success)
 * through the caller's return statement.
 *
 * Usage:
 *   SELFTEST_ASSERT(!selftest_poll_flag(&flag, 10, 20), 3);
 */
static inline i32 selftest_poll_flag(volatile bool *flag,
                                     i32 max_iters,
                                     i32 ms_per_iter)
{
    for (i32 i = 0; i < max_iters; i++) {
        if (__atomic_load_n(flag, __ATOMIC_ACQUIRE))
            return 0;
        ipi_send_broadcast(IPI_SCHED);
        sleep_ms(time_ms_new(ms_per_iter));
    }
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE) ? 0 : 1;
}

/* Integer variant: wait until *counter >= target. */
static inline i32 selftest_poll_count(volatile i32 *counter,
                                      i32 target,
                                      i32 max_iters,
                                      i32 ms_per_iter)
{
    for (i32 i = 0; i < max_iters; i++) {
        if (__atomic_load_n(counter, __ATOMIC_ACQUIRE) >= target)
            return 0;
        ipi_send_broadcast(IPI_SCHED);
        sleep_ms(time_ms_new(ms_per_iter));
    }
    return __atomic_load_n(counter, __ATOMIC_ACQUIRE) >= target ? 0 : 1;
}

/* Kick secondary harts out of WFI and yield to let spawned tasks run. */
#define SELFTEST_KICK_AND_YIELD(ms)    \
    do {                               \
        ipi_send_broadcast(IPI_SCHED); \
        sleep_ms(time_ms_new(ms));     \
    } while (0)

#endif /* MAZU_IPI_H */

#endif /* TESTS_COMMON_H */
