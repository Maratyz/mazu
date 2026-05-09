/* SPDX-License-Identifier: MIT */
/* Barrier self-tests.
 *
 * Test 1: 4 tasks barrier-wait; all proceed after last arrives.
 * Test 2: exactly one receives PTHREAD_BARRIER_SERIAL_THREAD.
 * Test 3: destroy while blocked returns EBUSY.
 * Test 4: cyclic reuse: two consecutive rounds.
 */

#include <kernel/sync/barrier.h>
#include <mazu/ipi.h>
#include <mazu/sched.h>
#include "tests-common.h"

/* Test 1+2: barrier synchronization and serial-thread. */
#define BARRIER_TEST_TASKS 4
static struct barrier test_barrier;
static volatile i32 barrier_arrived;
static volatile i32 barrier_serial_count;
static volatile i32 barrier_done_count;

static void barrier_worker(void *ctx __unused)
{
    __atomic_fetch_add(&barrier_arrived, 1, __ATOMIC_ACQ_REL);
    i32 ret = barrier_wait(&test_barrier);
    if (ret == PTHREAD_BARRIER_SERIAL_THREAD)
        __atomic_fetch_add(&barrier_serial_count, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&barrier_done_count, 1, __ATOMIC_ACQ_REL);
}

static i32 test_barrier_sync(void)
{
    barrier_init(&test_barrier, BARRIER_TEST_TASKS);
    __atomic_store_n(&barrier_arrived, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&barrier_serial_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&barrier_done_count, 0, __ATOMIC_RELEASE);

    /* Spawn N-1 workers; the test thread is the Nth. */
    for (i32 i = 0; i < BARRIER_TEST_TASKS - 1; i++) {
        struct result r = sched_create_task(barrier_worker, NULL);
        SELFTEST_ASSERT(!r.is_error, 1);
    }

    /* Let workers start. */
    SELFTEST_KICK_AND_YIELD(50);

    /* Act as the last thread to arrive. */
    __atomic_fetch_add(&barrier_arrived, 1, __ATOMIC_ACQ_REL);
    i32 ret = barrier_wait(&test_barrier);
    if (ret == PTHREAD_BARRIER_SERIAL_THREAD)
        __atomic_fetch_add(&barrier_serial_count, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&barrier_done_count, 1, __ATOMIC_ACQ_REL);

    /* Wait for all tasks to complete. */
    SELFTEST_ASSERT(
        !selftest_poll_count(&barrier_done_count, BARRIER_TEST_TASKS, 30, 50),
        2);

    /* All tasks must have arrived. */
    SELFTEST_ASSERT(__atomic_load_n(&barrier_arrived, __ATOMIC_ACQUIRE) ==
                        BARRIER_TEST_TASKS,
                    3);

    /* Exactly one serial-thread return across all tasks. */
    SELFTEST_ASSERT(
        __atomic_load_n(&barrier_serial_count, __ATOMIC_ACQUIRE) == 1, 4);

    return 0;
}
DEFINE_SELFTEST(barrier_sync, test_barrier_sync);

/* Test 3: destroy while blocked returns EBUSY. */
static struct barrier busy_barrier;
static volatile bool busy_worker_waiting;

static void busy_barrier_worker(void *ctx __unused)
{
    __atomic_store_n(&busy_worker_waiting, true, __ATOMIC_RELEASE);
    barrier_wait(&busy_barrier);
}

static i32 test_barrier_destroy_busy(void)
{
    barrier_init(&busy_barrier, 2);
    __atomic_store_n(&busy_worker_waiting, false, __ATOMIC_RELEASE);

    struct result r = sched_create_task(busy_barrier_worker, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    SELFTEST_ASSERT(!selftest_poll_flag(&busy_worker_waiting, 20, 50), 2);
    SELFTEST_KICK_AND_YIELD(20);

    /* Try to destroy while a worker is blocked. */
    i32 rc = barrier_destroy(&busy_barrier);
    SELFTEST_ASSERT(rc == -(i32) EBUSY, 3);

    /* Release the worker by completing the barrier ourselves. */
    barrier_wait(&busy_barrier);

    /* Poll for worker exit instead of a fixed sleep: CI QEMU schedules the
     * worker more slowly than a native host, and a single 50 ms yield isn't
     * always enough for it to leave barrier_wait.
     */
    rc = -(i32) EBUSY;
    for (i32 i = 0; i < 60 && rc != 0; i++) {
        SELFTEST_KICK_AND_YIELD(20);
        rc = barrier_destroy(&busy_barrier);
    }
    SELFTEST_ASSERT(rc == 0, 4);

    return 0;
}
DEFINE_SELFTEST(barrier_destroy_busy, test_barrier_destroy_busy);

/* Test 4: cyclic reuse — two consecutive rounds. */
static struct barrier cycle_barrier;
static volatile i32 cycle_round1;
static volatile i32 cycle_round2;
static volatile i32 cycle_done;

static void cycle_worker(void *ctx __unused)
{
    barrier_wait(&cycle_barrier);
    __atomic_fetch_add(&cycle_round1, 1, __ATOMIC_ACQ_REL);

    barrier_wait(&cycle_barrier);
    __atomic_fetch_add(&cycle_round2, 1, __ATOMIC_ACQ_REL);

    __atomic_fetch_add(&cycle_done, 1, __ATOMIC_ACQ_REL);
}

static i32 test_barrier_cyclic(void)
{
    barrier_init(&cycle_barrier, 2);
    __atomic_store_n(&cycle_round1, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&cycle_round2, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&cycle_done, 0, __ATOMIC_RELEASE);

    struct result r = sched_create_task(cycle_worker, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    SELFTEST_KICK_AND_YIELD(20);

    /* Round 1. */
    barrier_wait(&cycle_barrier);
    __atomic_fetch_add(&cycle_round1, 1, __ATOMIC_ACQ_REL);

    /* Poll for the worker's increment. A fixed 20 ms yield races on slow CI
     * QEMU; selftest_poll_count retries up to 60 * 20 ms.
     */
    SELFTEST_ASSERT(!selftest_poll_count(&cycle_round1, 2, 60, 20), 2);

    /* Round 2. */
    barrier_wait(&cycle_barrier);
    __atomic_fetch_add(&cycle_round2, 1, __ATOMIC_ACQ_REL);

    SELFTEST_ASSERT(!selftest_poll_count(&cycle_done, 1, 30, 50), 3);
    SELFTEST_ASSERT(__atomic_load_n(&cycle_round2, __ATOMIC_ACQUIRE) == 2, 4);

    return 0;
}
DEFINE_SELFTEST(barrier_cyclic, test_barrier_cyclic);
