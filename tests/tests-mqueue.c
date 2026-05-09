/* SPDX-License-Identifier: MIT */
/* Message queue self-tests.
 *
 * Test 1: basic send/receive round-trip.
 * Test 2: priority ordering: higher-priority messages dequeued first.
 * Test 3: blocking receive across tasks.
 * Test 4: timed receive timeout.
 */

#include <kernel/ipc/mqueue.h>
#include <mazu/ipi.h>
#include <mazu/sched.h>
#include "tests-common.h"

/* Test 1: basic send and receive. */
static i32 test_mq_basic(void)
{
    i32 h = mqueue_open(NULL, 4, 64);
    SELFTEST_ASSERT(h >= 0, 1);

    u8 msg[] = {0xAA, 0xBB, 0xCC};
    SELFTEST_ASSERT(mqueue_send(h, msg, sizeof(msg), 0) == 0, 2);

    u8 buf[64];
    u32 prio = 99;
    i32 len = mqueue_receive(h, buf, sizeof(buf), &prio);
    SELFTEST_ASSERT(len == 3, 3);
    SELFTEST_ASSERT(buf[0] == 0xAA && buf[1] == 0xBB && buf[2] == 0xCC, 4);
    SELFTEST_ASSERT(prio == 0, 5);

    mqueue_close(h);
    return 0;
}
DEFINE_SELFTEST(mq_basic, test_mq_basic);

/* Test 2: priority ordering. */
static i32 test_mq_priority(void)
{
    i32 h = mqueue_open(NULL, 8, 8);
    SELFTEST_ASSERT(h >= 0, 1);

    u8 lo = 1, mid = 2, hi = 3;
    mqueue_send(h, &lo, 1, 10);
    mqueue_send(h, &hi, 1, 30);
    mqueue_send(h, &mid, 1, 20);

    u8 buf;
    u32 prio;
    i32 len;

    len = mqueue_receive(h, &buf, 1, &prio);
    SELFTEST_ASSERT(len == 1 && buf == 3 && prio == 30, 2);

    len = mqueue_receive(h, &buf, 1, &prio);
    SELFTEST_ASSERT(len == 1 && buf == 2 && prio == 20, 3);

    len = mqueue_receive(h, &buf, 1, &prio);
    SELFTEST_ASSERT(len == 1 && buf == 1 && prio == 10, 4);

    mqueue_close(h);
    return 0;
}
DEFINE_SELFTEST(mq_priority, test_mq_priority);

/* Test 3: blocking receive across tasks. */
static volatile i32 mq_cross_handle;
static volatile bool mq_cross_received;
static volatile u8 mq_cross_data;

static void mq_receiver_task(void *ctx __unused)
{
    u8 buf;
    u32 prio;
    mqueue_receive((i32) mq_cross_handle, &buf, 1, &prio);
    __atomic_store_n(&mq_cross_data, buf, __ATOMIC_RELEASE);
    __atomic_store_n(&mq_cross_received, true, __ATOMIC_RELEASE);
}

static i32 test_mq_blocking(void)
{
    i32 h = mqueue_open(NULL, 4, 8);
    SELFTEST_ASSERT(h >= 0, 1);
    __atomic_store_n(&mq_cross_handle, h, __ATOMIC_RELEASE);
    __atomic_store_n(&mq_cross_received, false, __ATOMIC_RELEASE);

    struct result r = sched_create_task(mq_receiver_task, NULL);
    SELFTEST_ASSERT(!r.is_error, 2);

    SELFTEST_KICK_AND_YIELD(50);

    u8 val = 42;
    mqueue_send(h, &val, 1, 0);

    SELFTEST_ASSERT(!selftest_poll_flag(&mq_cross_received, 20, 50), 3);
    SELFTEST_ASSERT(__atomic_load_n(&mq_cross_data, __ATOMIC_ACQUIRE) == 42, 4);

    mqueue_close(h);
    return 0;
}
DEFINE_SELFTEST(mq_blocking, test_mq_blocking);

/* Test 4: timed receive timeout. */
static i32 test_mq_timedreceive(void)
{
    i32 h = mqueue_open(NULL, 4, 8);
    SELFTEST_ASSERT(h >= 0, 1);

    u8 buf;
    u32 prio;
    i32 ret = mqueue_timedreceive(h, &buf, 1, &prio, time_ms_new(50));
    SELFTEST_ASSERT(ret == -(i32) ETIMEDOUT, 2);

    mqueue_close(h);
    return 0;
}
DEFINE_SELFTEST(mq_timedreceive, test_mq_timedreceive);

static i32 test_mq_close_invalid(void)
{
    SELFTEST_ASSERT(mqueue_close(-1) == -(i32) EBADF, 1);

    i32 h = mqueue_open(NULL, 4, 8);
    SELFTEST_ASSERT(h >= 0, 2);
    SELFTEST_ASSERT(mqueue_close(h) == 0, 3);
    SELFTEST_ASSERT(mqueue_close(h) == -(i32) EBADF, 4);
    return 0;
}
DEFINE_SELFTEST(mq_close_invalid, test_mq_close_invalid);
