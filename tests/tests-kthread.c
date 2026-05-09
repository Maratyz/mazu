/* SPDX-License-Identifier: MIT */
#include <kernel/sched/kthread.h>
#include <mazu/selftest.h>

/* Test 1: create + stop.  Thread loops until should_stop. */
static int kthread_test_loop(void *arg __unused)
{
    while (!kthread_should_stop())
        sleep_ms(time_ms_new(5));
    return 42;
}

static i32 test_kthread_create_stop(void)
{
    struct kthread *kt =
        kthread_create(kthread_test_loop, NULL, "test_loop", SCHED_PRIO_NORMAL);
    if (!kt)
        return 1;

    /* Let it run a bit. */
    sleep_ms(time_ms_new(20));

    int ret = kthread_stop(kt);
    if (ret != 42)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kthread_create_stop, test_kthread_create_stop);

/* Test 2: natural exit.  Thread returns immediately; stop collects result. */
static int kthread_test_immediate(void *arg __unused)
{
    return 7;
}

static i32 test_kthread_natural_exit(void)
{
    struct kthread *kt = kthread_create(kthread_test_immediate, NULL,
                                        "test_imm", SCHED_PRIO_NORMAL);
    if (!kt)
        return 1;

    /* Let it run and exit. */
    sleep_ms(time_ms_new(20));

    int ret = kthread_stop(kt);
    if (ret != 7)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kthread_natural_exit, test_kthread_natural_exit);

/* Test 3: stop wakes a blocked kthread.  Thread blocks on a wq,
 * stop should cause it to eventually exit.
 */
static struct wait_queue_head kthread_test_wq;
static volatile bool kthread_test_wq_cond;

static int kthread_test_blocked(void *arg __unused)
{
    while (!kthread_should_stop()) {
        wait_event(kthread_test_wq,
                   kthread_test_wq_cond || kthread_should_stop());
        if (kthread_should_stop())
            break;
    }
    return 99;
}

static i32 test_kthread_stop_blocked(void)
{
    kthread_test_wq_cond = false;
    init_waitqueue_head(&kthread_test_wq);

    struct kthread *kt = kthread_create(kthread_test_blocked, NULL, "test_blk",
                                        SCHED_PRIO_NORMAL);
    if (!kt)
        return 1;

    /* Let the thread block on the wq. */
    sleep_ms(time_ms_new(20));

    /* Stop should cause the thread to see should_stop and exit.
     * Also set the wq condition to unblock the wait_event.
     */
    kthread_test_wq_cond = true;
    wake_up(&kthread_test_wq, 1);

    int ret = kthread_stop(kt);
    if (ret != 99)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kthread_stop_blocked, test_kthread_stop_blocked);
