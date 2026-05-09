/* SPDX-License-Identifier: MIT */

#include <mazu/proc.h>
#include <mazu/selftest.h>
#include "tests-common.h"

static i32 test_posix_timer_owner_isolation(void)
{
    struct proc *owner = proc_alloc();
    struct proc *other = proc_alloc();
    SELFTEST_ASSERT(owner != NULL, 1);
    SELFTEST_ASSERT(other != NULL, 2);

    i32 h = posix_timer_create(owner);
    SELFTEST_ASSERT(h >= 0, 3);

    SELFTEST_ASSERT(posix_timer_settime(h, other, 10, 0) == -(i32) EPERM, 4);
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
    SELFTEST_ASSERT(posix_timer_settime(h, owner, 50, 0) == 0, 3);
    SELFTEST_ASSERT(timer_pool[h].in_use, 4);
    SELFTEST_ASSERT(timer_pool[h].owner == owner, 5);

    proc_exit(owner, 0);

    SELFTEST_ASSERT(!timer_pool[h].in_use, 6);
    SELFTEST_ASSERT(!timer_pool[h].armed, 7);
    SELFTEST_ASSERT(timer_pool[h].owner == NULL, 8);
    SELFTEST_ASSERT(timer_pool[h].owner_generation == 0, 9);

    struct proc *reused = proc_alloc();
    SELFTEST_ASSERT(reused != NULL, 10);
    SELFTEST_ASSERT(posix_timer_settime(h, reused, 10, 0) == -(i32) EINVAL, 11);
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
    reused->sig_state.pending = 0;

    t->armed = true;
    t->interval_ticks = 0;
    t->owner = reused;
    t->owner_generation = owner_generation;
    timer_expiry_fn(t);

    SELFTEST_ASSERT(!t->armed, 6);
    SELFTEST_ASSERT((reused->sig_state.pending & BIT(SIGALRM)) == 0, 7);

    t->in_use = false;
    t->owner = NULL;
    t->owner_generation = 0;
    proc_set_state(reused, PROC_STATE_ZOMBIE);
    proc_free(reused);
    return 0;
}
DEFINE_SELFTEST(posix_timer_stale_owner_generation,
                test_posix_timer_stale_owner_generation);
