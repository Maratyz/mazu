/* SPDX-License-Identifier: MIT */
/* Reader-writer lock self-tests.
 *
 * Test 1: concurrent readers proceed simultaneously.
 * Test 2: writer blocks new readers.
 * Test 3: writer-preference: queued writer served before new reader.
 * Test 4: destroy while held returns EBUSY.
 */

#include <kernel/sync/rwlock.h>
#include <mazu/ipi.h>
#include <mazu/sched.h>
#include "tests-common.h"

/* Test 1: two readers can hold concurrently. */
static struct rwlock test_rw;
static volatile i32 rw_readers_active;
static volatile i32 rw_readers_max;
static volatile i32 rw_reader_done;

static void rw_reader_task(void *ctx __unused)
{
    rwlock_rdlock(&test_rw);
    i32 cur = __atomic_add_fetch(&rw_readers_active, 1, __ATOMIC_ACQ_REL);

    /* Track max concurrent readers. */
    i32 prev_max = __atomic_load_n(&rw_readers_max, __ATOMIC_ACQUIRE);
    while (cur > prev_max) {
        if (__atomic_compare_exchange_n(&rw_readers_max, &prev_max, cur, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }

    sleep_ms(time_ms_new(30));
    __atomic_fetch_sub(&rw_readers_active, 1, __ATOMIC_ACQ_REL);
    rwlock_unlock(&test_rw);
    __atomic_fetch_add(&rw_reader_done, 1, __ATOMIC_ACQ_REL);
}

static i32 test_rwlock_concurrent_readers(void)
{
    rwlock_init(&test_rw);
    __atomic_store_n(&rw_readers_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rw_readers_max, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rw_reader_done, 0, __ATOMIC_RELEASE);

    struct result r1 = sched_create_task(rw_reader_task, NULL);
    SELFTEST_ASSERT(!r1.is_error, 1);
    struct result r2 = sched_create_task(rw_reader_task, NULL);
    SELFTEST_ASSERT(!r2.is_error, 2);

    SELFTEST_ASSERT(!selftest_poll_count(&rw_reader_done, 2, 30, 50), 3);

    /* Both readers should have been active concurrently. */
    SELFTEST_ASSERT(__atomic_load_n(&rw_readers_max, __ATOMIC_ACQUIRE) >= 2, 4);
    return 0;
}
DEFINE_SELFTEST(rwlock_concurrent_readers, test_rwlock_concurrent_readers);

/* Test 2: writer blocks readers. */
static struct rwlock wr_rw;
static volatile bool wr_writer_held;
static volatile bool wr_reader_entered;
static volatile bool wr_reader_done_flag;

static void wr_reader(void *ctx __unused)
{
    rwlock_rdlock(&wr_rw);
    __atomic_store_n(&wr_reader_entered, true, __ATOMIC_RELEASE);
    rwlock_unlock(&wr_rw);
    __atomic_store_n(&wr_reader_done_flag, true, __ATOMIC_RELEASE);
}

static i32 test_rwlock_writer_blocks(void)
{
    rwlock_init(&wr_rw);
    __atomic_store_n(&wr_writer_held, false, __ATOMIC_RELEASE);
    __atomic_store_n(&wr_reader_entered, false, __ATOMIC_RELEASE);
    __atomic_store_n(&wr_reader_done_flag, false, __ATOMIC_RELEASE);

    /* Take write lock. */
    rwlock_wrlock(&wr_rw);

    /* Spawn a reader — it should block. */
    struct result r = sched_create_task(wr_reader, NULL);
    SELFTEST_ASSERT(!r.is_error, 1);
    SELFTEST_KICK_AND_YIELD(50);

    /* Reader should not have entered. */
    SELFTEST_ASSERT(!__atomic_load_n(&wr_reader_entered, __ATOMIC_ACQUIRE), 2);

    /* Release writer. */
    rwlock_unlock(&wr_rw);

    /* Reader should now complete. */
    SELFTEST_ASSERT(!selftest_poll_flag(&wr_reader_done_flag, 20, 50), 3);
    SELFTEST_ASSERT(__atomic_load_n(&wr_reader_entered, __ATOMIC_ACQUIRE), 4);

    return 0;
}
DEFINE_SELFTEST(rwlock_writer_blocks, test_rwlock_writer_blocks);

/* Test 3: destroy while held returns EBUSY. */
static i32 test_rwlock_destroy_busy(void)
{
    struct rwlock rw;
    rwlock_init(&rw);
    rwlock_rdlock(&rw);

    i32 rc = rwlock_destroy(&rw);
    SELFTEST_ASSERT(rc == -(i32) EBUSY, 1);

    rwlock_unlock(&rw);
    rc = rwlock_destroy(&rw);
    SELFTEST_ASSERT(rc == 0, 2);
    return 0;
}
DEFINE_SELFTEST(rwlock_destroy_busy, test_rwlock_destroy_busy);
