/* SPDX-License-Identifier: MIT */
/* Clock and nanosleep self-tests (item 15).
 *
 * Test 1: clock_gettime returns monotonically increasing values.
 * Test 2: clock_getres returns a positive resolution.
 * Test 3: nanosleep blocks for approximately the requested duration.
 */

#include <mazu/posix_time.h>
#include <mazu/sched.h>
#include <mazu/time.h>
#include "tests-common.h"

/* Kernel-side clock helpers — mirror the syscall logic for kernel selftests.
 * Real syscall testing requires user-space; these test the underlying logic.
 */

static i32 test_clock_monotonic(void)
{
    u64 t1 = time_rdtime();
    u64 freq = time_get_timebase_freq();
    SELFTEST_ASSERT(freq > 0, 1);

    /* Ensure time advances. */
    for (volatile i32 i = 0; i < 100; i++)
        ;

    u64 t2 = time_rdtime();
    SELFTEST_ASSERT(t2 > t1, 2);

    /* Convert to timespec and verify normalization. */
    struct timespec ts;
    ts.tv_sec = (i64) (t2 / freq);
    ts.tv_nsec = (i64) ((t2 % freq) * NSEC_PER_SEC / freq);
    SELFTEST_ASSERT(ts.tv_nsec >= 0 && ts.tv_nsec < NSEC_PER_SEC, 3);

    return 0;
}
DEFINE_SELFTEST(clock_monotonic, test_clock_monotonic);

static i32 test_clock_resolution(void)
{
    u64 freq = time_get_timebase_freq();
    SELFTEST_ASSERT(freq > 0, 1);

    i64 res_ns = (i64) (NSEC_PER_SEC / freq);
    if (res_ns == 0)
        res_ns = 1;
    SELFTEST_ASSERT(res_ns > 0, 2);
    /* Resolution should be better than 1ms on any reasonable hardware. */
    SELFTEST_ASSERT(res_ns < NSEC_PER_MSEC, 3);

    return 0;
}
DEFINE_SELFTEST(clock_resolution, test_clock_resolution);

static i32 test_nanosleep_accuracy(void)
{
    u64 before = time_rdtime();

    /* Sleep 50ms via the kernel sleep path. */
    sleep_ms(time_ms_new(50));

    u64 after = time_rdtime();
    u64 elapsed_us = time_ticks_to_us(after - before);

    /* Should have slept at least 40ms (allowing scheduler jitter). */
    SELFTEST_ASSERT(elapsed_us >= 40000, 1);
    /* Should not have overslept by more than 200ms. */
    SELFTEST_ASSERT(elapsed_us < 200000, 2);

    return 0;
}
DEFINE_SELFTEST(nanosleep_accuracy, test_nanosleep_accuracy);
