/* SPDX-License-Identifier: MIT */
/* Condition variable self-tests.
 *
 * Test 1: basic signal/wait handshake.
 * Test 2: broadcast wakes all waiters.
 * Test 3: timed wait returns ETIMEDOUT on expiry.
 */

#include <kernel/sync/condvar.h>
#include "tests-common.h"

/* Test 1: one producer signals, one consumer waits. */
static struct condvar cv_test_cv;
static struct pi_mutex cv_test_mtx;
static volatile bool cv_test_flag;

static void cv_test_producer(void *ctx __unused)
{
    sleep_ms(time_ms_new(50));

    pi_mutex_lock(&cv_test_mtx);
    cv_test_flag = true;
    condvar_signal(&cv_test_cv);
    pi_mutex_unlock(&cv_test_mtx);
}

static i32 test_condvar_basic(void)
{
    condvar_init(&cv_test_cv);
    pi_mutex_init(&cv_test_mtx);
    cv_test_flag = false;

    struct result r = sched_create_task(cv_test_producer, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    pi_mutex_lock(&cv_test_mtx);
    while (!cv_test_flag)
        condvar_wait(&cv_test_cv, &cv_test_mtx);
    pi_mutex_unlock(&cv_test_mtx);

    SELFTEST_ASSERT(cv_test_flag, 2);
    return 0;
}
DEFINE_SELFTEST(condvar_basic, test_condvar_basic);

/* Test 2: broadcast wakes two waiters. */
static struct condvar cv_bcast_cv;
static struct pi_mutex cv_bcast_mtx;
static volatile bool cv_bcast_flag;
static volatile i32 cv_bcast_count;
static volatile i32 cv_bcast_ready;

static void cv_bcast_waiter(void *ctx __unused)
{
    pi_mutex_lock(&cv_bcast_mtx);
    __atomic_fetch_add(&cv_bcast_ready, 1, __ATOMIC_RELAXED);
    while (!cv_bcast_flag)
        condvar_wait(&cv_bcast_cv, &cv_bcast_mtx);
    pi_mutex_unlock(&cv_bcast_mtx);
    __atomic_fetch_add(&cv_bcast_count, 1, __ATOMIC_RELAXED);
}

static i32 test_condvar_broadcast(void)
{
    condvar_init(&cv_bcast_cv);
    pi_mutex_init(&cv_bcast_mtx);
    cv_bcast_flag = false;
    cv_bcast_count = 0;
    cv_bcast_ready = 0;

    struct result r;
    r = sched_create_task(cv_bcast_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task(cv_bcast_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 2);

    /* Wait until both waiters have entered condvar_wait. */
    SELFTEST_ASSERT(!selftest_poll_count(&(cv_bcast_ready), 2, 60, 50), 3);
    SELFTEST_KICK_AND_YIELD(20);

    pi_mutex_lock(&cv_bcast_mtx);
    cv_bcast_flag = true;
    condvar_broadcast(&cv_bcast_cv);
    pi_mutex_unlock(&cv_bcast_mtx);

    SELFTEST_ASSERT(!selftest_poll_count(&(cv_bcast_count), 2, 60, 50), 4);
    SELFTEST_ASSERT(cv_bcast_count == 2, 5);
    return 0;
}
DEFINE_SELFTEST(condvar_broadcast, test_condvar_broadcast);

/* Test 3: timed wait expires and returns ETIMEDOUT. */
static struct condvar cv_tmo_cv;
static struct pi_mutex cv_tmo_mtx;

static i32 test_condvar_timeout(void)
{
    condvar_init(&cv_tmo_cv);
    pi_mutex_init(&cv_tmo_mtx);

    pi_mutex_lock(&cv_tmo_mtx);
    i32 rc = condvar_wait_timeout(&cv_tmo_cv, &cv_tmo_mtx, time_ms_new(100));
    pi_mutex_unlock(&cv_tmo_mtx);

    SELFTEST_ASSERT(rc == -(i32) ETIMEDOUT, 1);
    return 0;
}
DEFINE_SELFTEST(condvar_timeout, test_condvar_timeout);
