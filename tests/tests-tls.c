/* SPDX-License-Identifier: MIT */
/* TLS self-tests.
 *
 * Test 1: basic tls_set/tls_get round-trip on current task.
 * Test 2: two tasks set errno concurrently; values do not cross-contaminate.
 */

#include <mazu/ipi.h>
#include <mazu/sched.h>
#include "tests-common.h"

/* Test 1: basic TLS round-trip. */
static i32 test_tls_basic(void)
{
    tls_set(TLS_ENTRY_ERRNO, 0);
    SELFTEST_ASSERT(tls_get(TLS_ENTRY_ERRNO) == 0, 1);

    tls_set(TLS_ENTRY_ERRNO, 42);
    SELFTEST_ASSERT(tls_get(TLS_ENTRY_ERRNO) == 42, 2);

    tls_set(TLS_ENTRY_ERRNO, 0);
    return 0;
}
DEFINE_SELFTEST(tls_basic, test_tls_basic);

/* Test 2: two tasks write different errno values concurrently.
 * Each task checks that its own errno was not clobbered by the other.
 */
static volatile bool tls_worker_ready;
static volatile bool tls_worker_pass;
static volatile bool tls_worker_done;

#define TLS_TEST_ERRNO_A 77
#define TLS_TEST_ERRNO_B 99

static void tls_worker_task(void *ctx __unused)
{
    tls_set(TLS_ENTRY_ERRNO, TLS_TEST_ERRNO_B);
    __atomic_store_n(&tls_worker_ready, true, __ATOMIC_RELEASE);

    /* Spin briefly to overlap with main task's check. */
    for (i32 i = 0; i < 50; i++)
        sleep_ms(time_ms_new(1));

    bool ok = (tls_get(TLS_ENTRY_ERRNO) == TLS_TEST_ERRNO_B);
    __atomic_store_n(&tls_worker_pass, ok, __ATOMIC_RELEASE);
    __atomic_store_n(&tls_worker_done, true, __ATOMIC_RELEASE);
}

static i32 test_tls_cross_task(void)
{
    __atomic_store_n(&tls_worker_ready, false, __ATOMIC_RELEASE);
    __atomic_store_n(&tls_worker_pass, false, __ATOMIC_RELEASE);
    __atomic_store_n(&tls_worker_done, false, __ATOMIC_RELEASE);

    struct result r = sched_create_task(tls_worker_task, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    /* Set our own errno. */
    tls_set(TLS_ENTRY_ERRNO, TLS_TEST_ERRNO_A);

    /* Wait for worker to be ready. */
    SELFTEST_ASSERT(!selftest_poll_flag(&tls_worker_ready, 20, 50), 2);

    /* Our errno must still be A, not clobbered by worker's B. */
    SELFTEST_ASSERT(tls_get(TLS_ENTRY_ERRNO) == TLS_TEST_ERRNO_A, 3);

    /* Wait for worker to finish and verify it saw its own value. */
    SELFTEST_ASSERT(!selftest_poll_flag(&tls_worker_done, 60, 30), 4);
    SELFTEST_ASSERT(__atomic_load_n(&tls_worker_pass, __ATOMIC_ACQUIRE), 5);

    tls_set(TLS_ENTRY_ERRNO, 0);
    return 0;
}
DEFINE_SELFTEST(tls_cross_task, test_tls_cross_task);
