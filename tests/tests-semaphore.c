/* SPDX-License-Identifier: MIT */
/* Semaphore self-tests.
 *
 * Test 1: basic post/wait without blocking.
 * Test 2: trywait semantics.
 * Test 3: direct-handover: sem_post with waiter delivers directly,
 *          counter stays at 0.
 * Test 4: two threads contend; verify correct count via semaphore.
 */

#include <kernel/sync/semaphore.h>
#include "tests-common.h"

/* Test 1: sem_post then sem_wait does not block. */
static i32 test_sem_basic(void)
{
    struct semaphore sem;
    sem_init(&sem, 1);

    sem_wait(&sem);
    SELFTEST_ASSERT(sem.count == 0, 1);

    sem_post(&sem);
    SELFTEST_ASSERT(sem.count == 1, 2);

    return 0;
}
DEFINE_SELFTEST(sem_basic, test_sem_basic);

/* Test 2: trywait succeeds when count > 0, fails when 0. */
static i32 test_sem_trywait(void)
{
    struct semaphore sem;
    sem_init(&sem, 1);

    SELFTEST_ASSERT(sem_trywait(&sem) == 0, 1);
    SELFTEST_ASSERT(sem_trywait(&sem) == -(i32) EAGAIN, 2);

    return 0;
}
DEFINE_SELFTEST(sem_trywait, test_sem_trywait);

/* Test 3: direct handover, sem_post with blocked waiter keeps count at 0. */
static struct semaphore dh_sem;
static volatile bool dh_waiter_ran;
static volatile bool dh_waiter_ready;

static void dh_waiter_task(void *ctx __unused)
{
    dh_waiter_ready = true;
    sem_wait(&dh_sem); /* blocks: count is 0 */
    dh_waiter_ran = true;
}

static i32 test_sem_direct_handover(void)
{
    sem_init(&dh_sem, 0);
    dh_waiter_ran = false;
    dh_waiter_ready = false;

    struct result r = sched_create_task(dh_waiter_task, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    /* Wait until the waiter has started and entered sem_wait. */
    SELFTEST_ASSERT(!selftest_poll_flag(&(dh_waiter_ready), 20, 50), 2);
    SELFTEST_KICK_AND_YIELD(20);

    /* Post: should wake the waiter directly (counter stays 0). */
    sem_post(&dh_sem);

    SELFTEST_ASSERT(!selftest_poll_flag(&(dh_waiter_ran), 10, 20), 3);
    SELFTEST_ASSERT(dh_sem.count == 0, 4);

    return 0;
}
DEFINE_SELFTEST(sem_direct_handover, test_sem_direct_handover);

/* Test 4: two tasks use semaphore as mutex, increment shared counter.
 * Uses moderate iteration count to avoid overwhelming the SMP scheduler.
 */
static struct semaphore contend_sem;
static volatile i32 sem_contend_counter;
static volatile i32 sem_contend_done;
#define SEM_CONTEND_ITERS 10
static void sem_contend_worker(void *ctx __unused);

static void sem_count_workers(struct sched_task_info info, void *ctx)
{
    i32 *count = ctx;
    if (info.callback == sem_contend_worker)
        (*count)++;
}

static void sem_contend_worker(void *ctx __unused)
{
    for (i32 i = 0; i < SEM_CONTEND_ITERS; i++) {
        sem_wait(&contend_sem);
        sem_contend_counter++;
        sem_post(&contend_sem);
        sleep_ms(time_ms_new(0));
    }

    __atomic_fetch_add(&sem_contend_done, 1, __ATOMIC_RELAXED);
}

static i32 test_sem_contention(void)
{
    sem_init(&contend_sem, 1);
    sem_contend_counter = 0;
    sem_contend_done = 0;

    struct result r = sched_create_task(sem_contend_worker, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    for (i32 i = 0; i < SEM_CONTEND_ITERS; i++) {
        sem_wait(&contend_sem);
        sem_contend_counter++;
        sem_post(&contend_sem);
        sleep_ms(time_ms_new(0));
    }

    SELFTEST_ASSERT(!selftest_poll_count(&(sem_contend_done), 1, 60, 30), 2);

    /* Wait for worker task to fully exit. */
    for (i32 i = 0; i < 20; i++) {
        i32 workers = 0;
        sched_for_each_task(sem_count_workers, &workers);
        if (workers == 0)
            break;
        SELFTEST_KICK_AND_YIELD(10);
    }

    SELFTEST_ASSERT(sem_contend_counter == 2 * SEM_CONTEND_ITERS, 3);
    SELFTEST_ASSERT(sem_contend_done == 1, 4);

    i32 workers = 0;
    sched_for_each_task(sem_count_workers, &workers);
    SELFTEST_ASSERT(workers == 0, 5);

    return 0;
}
DEFINE_SELFTEST(sem_contention, test_sem_contention);
