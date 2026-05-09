/* SPDX-License-Identifier: MIT */
#include <kernel/sched/waitqueue.h>
#include "tests-common.h"

/* Test 1: basic wait + wake.  Spawn a task that sets a flag and wakes
 * the wait queue.  Main task waits on the flag.
 */
static volatile bool wq_test_flag;
static struct wait_queue_head wq_test_head;

static void wq_test_setter(void *ctx __unused)
{
    wq_test_flag = true;
    wake_up(&wq_test_head, 1);
}

static i32 test_wait_queue_basic(void)
{
    wq_test_flag = false;
    init_waitqueue_head(&wq_test_head);

    struct result r = sched_create_task(wq_test_setter, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    wait_event(wq_test_head, wq_test_flag);
    SELFTEST_ASSERT(wq_test_flag, 2);
    return 0;
}
DEFINE_SELFTEST(wait_queue_basic, test_wait_queue_basic);

/* Test 2: wake_up(wq, 0) wakes all waiters.  Two waiter tasks block;
 * the main test task (selftest runner) sets the flag and wakes all.
 */
static volatile i32 wq_wake_all_count;
static volatile i32 wq_wake_all_ready;
static volatile bool wq_wake_all_flag;
static struct wait_queue_head wq_wake_all_head;

static void wq_wake_all_waiter(void *ctx __unused)
{
    __atomic_fetch_add(&wq_wake_all_ready, 1, __ATOMIC_RELAXED);
    wait_event(wq_wake_all_head, wq_wake_all_flag);
    __atomic_fetch_add(&wq_wake_all_count, 1, __ATOMIC_RELAXED);
}

static i32 test_wait_queue_wake_all(void)
{
    wq_wake_all_count = 0;
    wq_wake_all_ready = 0;
    wq_wake_all_flag = false;
    init_waitqueue_head(&wq_wake_all_head);

    struct result r;
    r = sched_create_task(wq_wake_all_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task(wq_wake_all_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 2);

    /* Wait until both waiters have entered wait_event. */
    SELFTEST_ASSERT(!selftest_poll_count(&(wq_wake_all_ready), 2, 20, 50), 3);
    /* Extra kick to ensure both are blocked inside prepare_to_wait. */
    SELFTEST_KICK_AND_YIELD(20);

    /* Wake all waiters from the main task context. */
    wq_wake_all_flag = true;
    wake_up(&wq_wake_all_head, 0);

    SELFTEST_ASSERT(!selftest_poll_count(&(wq_wake_all_count), 2, 40, 50), 4);
    SELFTEST_ASSERT(wq_wake_all_count == 2, 5);
    return 0;
}
DEFINE_SELFTEST(wait_queue_wake_all, test_wait_queue_wake_all);

/* Test 3: no lost wakeup.  Wake fires before the task yields.
 * The task should proceed without blocking.
 */
static volatile bool wq_no_lost_flag;
static struct wait_queue_head wq_no_lost_head;

static i32 test_wait_queue_no_lost_wakeup(void)
{
    wq_no_lost_flag = false;
    init_waitqueue_head(&wq_no_lost_head);

    /* Set the condition BEFORE entering wait_event. */
    wq_no_lost_flag = true;

    /* wait_event checks condition first; should not block at all. */
    wait_event(wq_no_lost_head, wq_no_lost_flag);
    return 0;
}
DEFINE_SELFTEST(wait_queue_no_lost_wakeup, test_wait_queue_no_lost_wakeup);

/* Test 4: wake policy is priority-ordered (highest priority first). */
static volatile bool wq_prio_flag;
static volatile i32 wq_prio_order[2];
static volatile i32 wq_prio_idx;
static volatile i32 wq_prio_ready;
static struct wait_queue_head wq_prio_head;

static void wq_prio_waiter(void *ctx)
{
    i32 slot = (i32) (uptr) ctx;
    __atomic_fetch_add(&wq_prio_ready, 1, __ATOMIC_RELAXED);
    wait_event(wq_prio_head, wq_prio_flag);
    i32 idx = __atomic_fetch_add(&wq_prio_idx, 1, __ATOMIC_RELAXED);
    if (idx >= 0 && idx < (i32) countof(wq_prio_order))
        wq_prio_order[idx] = slot;
}

static i32 test_wait_queue_priority_wake(void)
{
    wq_prio_flag = false;
    wq_prio_idx = 0;
    wq_prio_ready = 0;
    wq_prio_order[0] = -1;
    wq_prio_order[1] = -1;
    init_waitqueue_head(&wq_prio_head);

    /* Create lower-priority waiter first, then higher-priority waiter. */
    struct result r = sched_create_task_prio(wq_prio_waiter, (void *) (uptr) 1,
                                             SCHED_PRIO_NORMAL);
    SELFTEST_ASSERT(!r.is_error, 1);
    r = sched_create_task_prio(wq_prio_waiter, (void *) (uptr) 2,
                               SCHED_PRIO_HIGH);
    SELFTEST_ASSERT(!r.is_error, 2);

    /* Wait until both waiters have entered wait_event. */
    SELFTEST_ASSERT(!selftest_poll_count(&(wq_prio_ready), 2, 20, 50), 3);
    SELFTEST_KICK_AND_YIELD(20);

    /* Wake one: higher-priority waiter must run first. */
    wq_prio_flag = true;
    wake_up(&wq_prio_head, 1);
    SELFTEST_ASSERT(!selftest_poll_count(&(wq_prio_idx), 1, 40, 50), 4);
    SELFTEST_ASSERT(wq_prio_order[0] == 2, 5);

    wake_up(&wq_prio_head, 0);
    SELFTEST_ASSERT(!selftest_poll_count(&(wq_prio_idx), 2, 40, 50), 6);
    SELFTEST_ASSERT(wq_prio_order[1] == 1, 7);

    return 0;
}
DEFINE_SELFTEST(wait_queue_priority_wake, test_wait_queue_priority_wake);

/* Test 5: unblock reason is preserved across wake path. */
static volatile bool wq_reason_flag;
static volatile bool wq_reason_ready;
static volatile enum wait_unblock_reason wq_reason_seen;
static struct wait_queue_head wq_reason_head;

static void wq_reason_waiter(void *ctx __unused)
{
    struct wait_queue_entry wqe;
    wqe.task = sched_current_task();
    wqe.reason = WAIT_UNBLOCK_NONE;
    list_init(&wqe.node);

    while (!wq_reason_flag) {
        prepare_to_wait(&wq_reason_head, &wqe);
        /* Signal readiness AFTER linking into the queue so the main
         * task's wake_up_cancel always finds us queued.
         */
        wq_reason_ready = true;
        if (wq_reason_flag || wait_unblock_is_terminal(wqe.reason))
            break;
        sched_yield_trap();
        if (wait_unblock_is_terminal(wqe.reason))
            break;
    }
    finish_wait(&wq_reason_head, &wqe);
    wq_reason_seen = wqe.reason;
}

static i32 test_wait_queue_reason_preserve(void)
{
    wq_reason_flag = false;
    wq_reason_ready = false;
    wq_reason_seen = WAIT_UNBLOCK_NONE;
    init_waitqueue_head(&wq_reason_head);

    struct result r = sched_create_task(wq_reason_waiter, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);

    /* Wait until the waiter has entered wait_event. */
    SELFTEST_ASSERT(!selftest_poll_flag(&(wq_reason_ready), 20, 50), 2);
    SELFTEST_KICK_AND_YIELD(20);

    wake_up_cancel(&wq_reason_head, 1);

    /* Wait for the waiter to observe the cancel reason. */
    for (i32 i = 0; i < 40 && wq_reason_seen == WAIT_UNBLOCK_NONE; i++) {
        ipi_send_broadcast(IPI_SCHED);
        sleep_ms(time_ms_new(50));
    }

    SELFTEST_ASSERT(wq_reason_seen == WAIT_UNBLOCK_CANCEL, 3);
    return 0;
}
DEFINE_SELFTEST(wait_queue_reason_preserve, test_wait_queue_reason_preserve);
