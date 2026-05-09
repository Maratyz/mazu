/* SPDX-License-Identifier: MIT */
/* PI mutex self-tests.
 *
 * Test 1: uncontended lock/unlock.
 * Test 2: trylock succeeds when free, fails when held.
 * Test 3: priority inheritance, low-priority holder is boosted.
 * Test 4: two tasks contend; correct count via mutex.
 * Test 5: multi-mutex priority recomputation, unlocking one mutex
 *          retains boost from another mutex's waiters.
 */

#include <kernel/sync/mutex.h>
#include "tests-common.h"

/* Test 1: basic uncontended lock/unlock cycle. */
static i32 test_mutex_basic(void)
{
    struct pi_mutex mtx;
    pi_mutex_init(&mtx);

    pi_mutex_lock(&mtx);
    if (mtx.owner != sched_current_task()) {
        pi_mutex_unlock(&mtx);
        return 1;
    }

    pi_mutex_unlock(&mtx);
    SELFTEST_ASSERT(mtx.owner == NULL, 2);

    return 0;
}
DEFINE_SELFTEST(mutex_basic, test_mutex_basic);

/* Test 2: trylock semantics. */
static i32 test_mutex_trylock(void)
{
    struct pi_mutex mtx;
    pi_mutex_init(&mtx);

    if (pi_mutex_trylock(&mtx) != 0)
        return 1;

    /* Second trylock must fail (held by self). */
    if (pi_mutex_trylock(&mtx) != -(i32) EBUSY) {
        pi_mutex_unlock(&mtx);
        return 2;
    }

    pi_mutex_unlock(&mtx);
    return 0;
}
DEFINE_SELFTEST(mutex_trylock, test_mutex_trylock);

/* Test 3: priority inheritance, low-priority holder is boosted. */
static struct pi_mutex pi_test_mtx;
static volatile bool pi_test_holder_locked;
static volatile bool pi_test_holder_boosted;
static volatile bool pi_test_done;

static void pi_test_low_prio_holder(void *ctx __unused)
{
    pi_mutex_lock(&pi_test_mtx);
    pi_test_holder_locked = true;

    /* Let the high-priority task run and contend. */
    SELFTEST_KICK_AND_YIELD(80);

    /* Check if the priority was boosted above NORMAL. */
    struct sched_task *self = sched_current_task();
    pi_test_holder_boosted = (self->td_prio > SCHED_PRIO_NORMAL);

    pi_mutex_unlock(&pi_test_mtx);
}

static void pi_test_high_prio_waiter(void *ctx __unused)
{
    /* Wait until the holder has acquired the mutex. */
    while (!pi_test_holder_locked)
        SELFTEST_KICK_AND_YIELD(10);

    /* This will block and trigger PI boost on the holder. */
    pi_mutex_lock(&pi_test_mtx);
    pi_mutex_unlock(&pi_test_mtx);

    pi_test_done = true;
}

static i32 test_mutex_priority_inheritance(void)
{
    pi_mutex_init(&pi_test_mtx);
    pi_test_holder_locked = false;
    pi_test_holder_boosted = false;
    pi_test_done = false;

    struct result r;
    r = sched_create_task_prio(pi_test_low_prio_holder, NULL,
                               SCHED_PRIO_NORMAL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task_prio(pi_test_high_prio_waiter, NULL, SCHED_PRIO_HIGH);
    SELFTEST_ASSERT(!r.is_error, 2);

    SELFTEST_ASSERT(!selftest_poll_flag(&(pi_test_done), 20, 50), 3);
    SELFTEST_ASSERT(pi_test_holder_boosted, 4);

    return 0;
}
DEFINE_SELFTEST(mutex_priority_inheritance, test_mutex_priority_inheritance);

/* Test 4: two tasks contend on mutex, increment shared counter.
 * Result must equal exactly 2 * iterations.
 */
static struct pi_mutex contend_mtx;
static volatile i32 contend_counter;
#define CONTEND_ITERS 10

static void contend_worker(void *ctx __unused)
{
    for (i32 i = 0; i < CONTEND_ITERS; i++) {
        pi_mutex_lock(&contend_mtx);
        __atomic_fetch_add(&contend_counter, 1, __ATOMIC_RELAXED);
        pi_mutex_unlock(&contend_mtx);
    }
}

static i32 test_mutex_contention(void)
{
    pi_mutex_init(&contend_mtx);
    contend_counter = 0;

    struct result r;
    r = sched_create_task(contend_worker, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task(contend_worker, NULL);
    SELFTEST_ASSERT(!r.is_error, 2);

    SELFTEST_ASSERT(
        !selftest_poll_count(&(contend_counter), 2 * CONTEND_ITERS, 60, 30), 3);
    SELFTEST_ASSERT(__atomic_load_n(&contend_counter, __ATOMIC_ACQUIRE) ==
                        2 * CONTEND_ITERS,
                    4);

    return 0;
}
DEFINE_SELFTEST(mutex_contention, test_mutex_contention);

/* Test 5: multi-mutex priority recomputation.
 * Task holds two mutexes (M1 and M2).  A high-priority task contends
 * on M1, boosting the holder.  Unlocking M2 must NOT drop the boost
 * inherited from M1's waiter.
 */
static struct pi_mutex multi_mtx1;
static struct pi_mutex multi_mtx2;
static volatile bool multi_holder_locked;
static volatile bool multi_holder_kept_boost;
static volatile bool multi_done;

static void multi_holder_task(void *ctx __unused)
{
    pi_mutex_lock(&multi_mtx1);
    pi_mutex_lock(&multi_mtx2);
    multi_holder_locked = true;

    /* Let the high-priority task run and contend on M1. */
    SELFTEST_KICK_AND_YIELD(80);

    /* Unlock M2 first; priority should remain boosted because
     * M1 still has a high-priority waiter.
     */
    pi_mutex_unlock(&multi_mtx2);

    struct sched_task *self = sched_current_task();
    multi_holder_kept_boost = (self->td_prio > SCHED_PRIO_NORMAL);

    pi_mutex_unlock(&multi_mtx1);
}

static void multi_waiter_task(void *ctx __unused)
{
    while (!multi_holder_locked)
        SELFTEST_KICK_AND_YIELD(10);

    /* Block on M1, triggering PI boost on the holder. */
    pi_mutex_lock(&multi_mtx1);
    pi_mutex_unlock(&multi_mtx1);

    multi_done = true;
}

static i32 test_mutex_multi_prio(void)
{
    pi_mutex_init(&multi_mtx1);
    pi_mutex_init(&multi_mtx2);
    multi_holder_locked = false;
    multi_holder_kept_boost = false;
    multi_done = false;

    struct result r;
    r = sched_create_task_prio(multi_holder_task, NULL, SCHED_PRIO_NORMAL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task_prio(multi_waiter_task, NULL, SCHED_PRIO_HIGH);
    SELFTEST_ASSERT(!r.is_error, 2);

    SELFTEST_ASSERT(!selftest_poll_flag(&(multi_done), 40, 50), 3);
    SELFTEST_ASSERT(multi_holder_kept_boost, 4);

    return 0;
}
DEFINE_SELFTEST(mutex_multi_prio, test_mutex_multi_prio);

/* Test 6: a task that exits while holding a mutex must release ownership so a
 * blocked waiter can acquire it.
 */
static struct pi_mutex owner_exit_mtx;
static volatile bool owner_exit_waiter_done;

static void owner_exit_holder(void *ctx __unused)
{
    pi_mutex_lock(&owner_exit_mtx);
    /* Return without unlocking; deferred cleanup must release the mutex. */
}

static void owner_exit_waiter(void *ctx __unused)
{
    pi_mutex_lock(&owner_exit_mtx);
    pi_mutex_unlock(&owner_exit_mtx);
    owner_exit_waiter_done = true;
}

static i32 test_mutex_owner_exit_release(void)
{
    pi_mutex_init(&owner_exit_mtx);
    owner_exit_waiter_done = false;

    struct result r;
    r = sched_create_task(owner_exit_holder, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task(owner_exit_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 2);

    SELFTEST_ASSERT(!selftest_poll_flag(&(owner_exit_waiter_done), 40, 50), 3);
    SELFTEST_ASSERT(owner_exit_mtx.owner == NULL, 4);

    return 0;
}
DEFINE_SELFTEST(mutex_owner_exit_release, test_mutex_owner_exit_release);
