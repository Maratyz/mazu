/* SPDX-License-Identifier: MIT */

#include <mazu/kvalloc.h>
#include <mazu/proc.h>
#include <mazu/selftest.h>
#include "tests-common.h"

static struct sched_task *alloc_mock_task(void)
{
    struct option_byte_array td_mem =
        kvalloc_alloc(sizeof(struct sched_task), alignof(struct sched_task));
    if (td_mem.is_none)
        return NULL;
    struct sched_task *td = byte_array_ptr(option_byte_array_checked(td_mem));
    memset(td, 0, sizeof(*td));
    return td;
}

static void free_mock_task(struct sched_task *td)
{
    kvalloc_free(byte_array_new((byte *) td, sizeof(*td)));
}

static bool attach_mock_task(struct proc *p, struct sched_task *td)
{
    u64 flags = proc_table_lock_irqsave();
    bool ok = proc_attach_task(p, td);
    proc_table_unlock_irqrestore(flags);
    return ok;
}

static i32 test_posix_timer_owner_isolation(void)
{
    struct proc *owner = proc_alloc();
    struct proc *other = proc_alloc();
    SELFTEST_ASSERT(owner != NULL, 1);
    SELFTEST_ASSERT(other != NULL, 2);

    i32 h = posix_timer_create(owner);
    SELFTEST_ASSERT(h >= 0, 3);

    SELFTEST_ASSERT(posix_timer_settime(h, other, 10, 0, 0) == -(i32) EPERM, 4);
    SELFTEST_ASSERT(posix_timer_gettime(h, other) == -(i64) EPERM, 5);
    SELFTEST_ASSERT(posix_timer_getoverrun(h, other) == -(i64) EPERM, 6);
    SELFTEST_ASSERT(posix_timer_delete(h, other) == -(i32) EPERM, 7);
    SELFTEST_ASSERT(posix_timer_delete(h, owner) == 0, 8);

    proc_free(owner);
    proc_free(other);
    return 0;
}
DEFINE_SELFTEST(posix_timer_owner_isolation, test_posix_timer_owner_isolation);

static i32 test_posix_timer_teardown_on_proc_exit(void)
{
    struct proc *owner = proc_alloc();
    SELFTEST_ASSERT(owner != NULL, 1);
    proc_set_state(owner, PROC_STATE_RUNNING);

    i32 h = posix_timer_create(owner);
    SELFTEST_ASSERT(h >= 0, 2);
    SELFTEST_ASSERT(posix_timer_settime(h, owner, 50, 0, 0) == 0, 3);
    SELFTEST_ASSERT(timer_pool[h].in_use, 4);
    SELFTEST_ASSERT(timer_pool[h].owner == owner, 5);

    proc_exit(owner, 0);

    SELFTEST_ASSERT(!timer_pool[h].in_use, 6);
    SELFTEST_ASSERT(!timer_pool[h].armed, 7);
    SELFTEST_ASSERT(timer_pool[h].owner == NULL, 8);
    SELFTEST_ASSERT(timer_pool[h].owner_generation == 0, 9);

    struct proc *reused = proc_alloc();
    SELFTEST_ASSERT(reused != NULL, 10);
    SELFTEST_ASSERT(posix_timer_settime(h, reused, 10, 0, 0) == -(i32) EINVAL,
                    11);
    proc_free(reused);
    return 0;
}
DEFINE_SELFTEST(posix_timer_teardown_on_proc_exit,
                test_posix_timer_teardown_on_proc_exit);

static i32 test_posix_timer_stale_owner_generation(void)
{
    struct proc *owner = proc_alloc();
    SELFTEST_ASSERT(owner != NULL, 1);

    i32 h = posix_timer_create(owner);
    SELFTEST_ASSERT(h >= 0, 2);

    struct posix_timer *t = &timer_pool[h];
    SELFTEST_ASSERT(t->owner == owner, 3);
    u32 owner_generation = owner->generation;

    proc_free(owner);

    struct proc *reused = proc_alloc();
    SELFTEST_ASSERT(reused != NULL, 4);
    SELFTEST_ASSERT(reused == owner, 5);
    proc_set_state(reused, PROC_STATE_RUNNING);
    /* No task is attached to the reused proc here, so the per-thread
     * pending mask is unreachable. timer_expiry_fn looking up the
     * thread-group leader will get NULL, so no overrun is counted and
     * no signal is delivered. Verify by checking !t->armed below.
     */

    t->armed = true;
    t->interval_ticks = 0;
    t->owner = reused;
    t->owner_generation = owner_generation;
    timer_expiry_fn(t);

    SELFTEST_ASSERT(!t->armed, 6);

    t->in_use = false;
    t->owner = NULL;
    t->owner_generation = 0;
    proc_set_state(reused, PROC_STATE_ZOMBIE);
    proc_free(reused);
    return 0;
}
DEFINE_SELFTEST(posix_timer_stale_owner_generation,
                test_posix_timer_stale_owner_generation);

static i32 test_posix_timer_rejects_exited_thread_target(void)
{
    struct proc *owner = proc_alloc();
    SELFTEST_ASSERT(owner != NULL, 1);
    proc_set_state(owner, PROC_STATE_RUNNING);

    struct sched_task *target = alloc_mock_task();
    SELFTEST_ASSERT(target != NULL, 2);
    target->proc = owner;
    target->id = 7;
    target->td_join_state = TD_JOIN_EXITED;
    SELFTEST_ASSERT(attach_mock_task(owner, target), 3);

    i32 h = posix_timer_create(owner);
    SELFTEST_ASSERT(h >= 0, 4);
    SELFTEST_ASSERT(
        posix_timer_settime(h, owner, 10, 0, target->id) == -(i32) ESRCH, 5);
    SELFTEST_ASSERT(timer_pool[h].target_tid == 0, 6);
    SELFTEST_ASSERT(posix_timer_delete(h, owner) == 0, 7);

    u64 flags = proc_table_lock_irqsave();
    proc_detach_task(owner, target);
    proc_table_unlock_irqrestore(flags);
    free_mock_task(target);
    proc_set_state(owner, PROC_STATE_ZOMBIE);
    proc_free(owner);
    return 0;
}
DEFINE_SELFTEST(posix_timer_rejects_exited_thread_target,
                test_posix_timer_rejects_exited_thread_target);
