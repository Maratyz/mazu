/* SPDX-License-Identifier: MIT */
/* Priority-inheriting mutex.
 *
 * Waiters are kept in descending priority order.  When a task blocks, its
 * priority is compared with the owner's; if higher, the owner is boosted. On
 * unlock, the highest-priority waiter (head of the list) receives direct
 * ownership handover (no barging window), and the old owner's priority is
 * recomputed from any remaining held PI mutexes.
 *
 * Single-level PI: if A holds mutex M1 and blocks on M2 held by B, B is NOT
 * transitively boosted. Transitive PI requires a waiter chain walk and is
 * deferred to future work if needed.
 */

#include "mutex.h"
#include <mazu/assert.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include "../lockdep.h"
#include "../proc/signal.h"

struct pi_mutex_waiter {
    struct sched_task *task;
    struct list_head node;
    bool granted; /* set by unlock to signal direct ownership handover */
};

struct pi_mutex_cleanup_ctx {
    struct pi_mutex *mtx;
    struct pi_mutex_waiter *waiter;
};

/* Recompute a task's effective priority from its base priority and the
 * cached top_waiter_prio of each PI mutex it still holds.
 *
 * Each mutex's top_waiter_prio is maintained under that mutex's spinlock
 * whenever waiters are added or removed, so this function never needs to
 * traverse foreign waiter lists or acquire foreign locks.
 */
static void pi_recompute_prio(struct sched_task *task)
{
    u8 max_prio = task->td_base_prio;
    struct pi_mutex *m;

    list_for_each_entry_safe (&task->pi_held_mutexes, m, struct pi_mutex,
                              pi_held) {
        /* Relaxed load: foreign mutexes in the held chain may have their
         * top_waiter_prio updated concurrently under their own lock.  A
         * stale read is transient — the next unlock/lock cycle on that
         * mutex will trigger another recomputation and converge.
         */
        u8 wp = __atomic_load_n(&m->top_waiter_prio, __ATOMIC_RELAXED);
        if (wp > max_prio)
            max_prio = wp;
    }
    task->td_prio = max_prio;
}

void pi_mutex_refresh_prio(struct sched_task *task)
{
    if (!task)
        return;
    pi_recompute_prio(task);
}

static void pi_mutex_wait_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct pi_mutex_cleanup_ctx *ctx = ctx_ptr;
    assert(task);
    assert(ctx);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->mtx->lock);
    if (ctx->waiter->node.next != &ctx->waiter->node)
        list_del_init(&ctx->waiter->node);

    if (list_empty(&ctx->mtx->waiters))
        ctx->mtx->top_waiter_prio = 0;
    else {
        struct pi_mutex_waiter *top =
            list_entry(ctx->mtx->waiters.next, struct pi_mutex_waiter, node);
        ctx->mtx->top_waiter_prio = top->task->td_prio;
    }

    if (ctx->mtx->owner)
        pi_recompute_prio(ctx->mtx->owner);

    spin_unlock_irqrestore(&ctx->mtx->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

void pi_mutex_init(struct pi_mutex *mtx)
{
    mtx->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    mtx->owner = NULL;
    list_init(&mtx->waiters);
    list_init(&mtx->pi_held);
    mtx->top_waiter_prio = 0;
}

i32 pi_mutex_lock_interruptible(struct pi_mutex *mtx)
{
    struct sched_task *self = sched_current_task();
    struct pi_mutex_waiter w = {.task = self, .granted = false};
    struct pi_mutex_cleanup_ctx cleanup = {
        .mtx = mtx,
        .waiter = &w,
    };
    list_init(&w.node);
    DEBUG_ASSERT(!in_interrupt_context());

    for (;;) {
        lockdep_acquire(LOCK_LEVEL_WAITQ);
        u64 flags = spin_lock_irqsave(&mtx->lock);

        i32 abort = wait_abort_error_current();
        if (abort < 0 && w.node.next != &w.node && !w.granted) {
            list_del_init(&w.node);
            if (list_empty(&mtx->waiters))
                mtx->top_waiter_prio = 0;
            else {
                struct pi_mutex_waiter *top =
                    list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
                mtx->top_waiter_prio = top->task->td_prio;
            }
            if (mtx->owner)
                pi_recompute_prio(mtx->owner);
            sched_clear_block_cleanup(self);
            if (self->state == TD_STATE_BLOCKED)
                sched_set_task_state(self, TD_STATE_RUNNING);
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return abort;
        }

        /* Direct handover: unlock (or release_all) transferred ownership.
         * release_all already links pi_held; normal unlock does not.
         * Only add if not already linked (pi_held.next == &pi_held means
         * the node was init'd but not linked).
         */
        if (w.granted) {
            sched_clear_block_cleanup(self);
            if (mtx->pi_held.next == &mtx->pi_held)
                list_add(&self->pi_held_mutexes, &mtx->pi_held);
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return 0;
        }

        /* Non-recursive mutex: locking while already held by self is a bug
         * because the subsequent unlock would release prematurely.
         */
        ALWAYS_ASSERT(mtx->owner != self);

        if (!mtx->owner) {
            if (w.node.next != &w.node) {
                list_del_init(&w.node);
                /* Refresh cached top priority after self-removal. */
                if (list_empty(&mtx->waiters))
                    mtx->top_waiter_prio = 0;
                else {
                    struct pi_mutex_waiter *top = list_entry(
                        mtx->waiters.next, struct pi_mutex_waiter, node);
                    mtx->top_waiter_prio = top->task->td_prio;
                }
            }
            mtx->owner = self;
            sched_clear_block_cleanup(self);
            list_add(&self->pi_held_mutexes, &mtx->pi_held);
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return 0;
        }

        /* Contended: boost owner if the caller's priority is higher. */
        if (self->td_prio > mtx->owner->td_prio)
            mtx->owner->td_prio = self->td_prio;

        /* Insert into waiter list in descending priority order once. */
        if (w.node.next == &w.node) {
            struct list_head *pos = &mtx->waiters;
            struct pi_mutex_waiter *it;
            list_for_each_entry_safe (&mtx->waiters, it, struct pi_mutex_waiter,
                                      node) {
                if (it->task->td_prio < self->td_prio) {
                    pos = &it->node;
                    break;
                }
            }
            list_add_tail(pos, &w.node);

            /* Update cached top priority: head of the sorted list. */
            struct pi_mutex_waiter *top =
                list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
            mtx->top_waiter_prio = top->task->td_prio;
        }
        sched_set_task_state(self, TD_STATE_BLOCKED);
        sched_set_block_cleanup(self, pi_mutex_wait_cleanup, &cleanup);

        spin_unlock_irqrestore(&mtx->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        sched_yield_trap();
    }
}

void pi_mutex_lock(struct pi_mutex *mtx)
{
    i32 rc = pi_mutex_lock_interruptible(mtx);
    ALWAYS_ASSERT(rc == 0);
}

i32 pi_mutex_trylock(struct pi_mutex *mtx)
{
    struct sched_task *self = sched_current_task();

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&mtx->lock);

    if (mtx->owner) {
        spin_unlock_irqrestore(&mtx->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i32) EBUSY;
    }

    mtx->owner = self;
    list_add(&self->pi_held_mutexes, &mtx->pi_held);
    spin_unlock_irqrestore(&mtx->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

void pi_mutex_unlock(struct pi_mutex *mtx)
{
    struct sched_task *self = sched_current_task();

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&mtx->lock);

    /* Check ownership under the lock so unlock stays serialized with
     * concurrent lockers and wakeups on other harts.
     */
    ALWAYS_ASSERT(mtx->owner == self);

    /* Remove this mutex from the owner's held chain before recomputing
     * priority.  This mutex's waiters should no longer influence old owner's
     * boost level.
     */
    list_del_init(&mtx->pi_held);
    pi_recompute_prio(self);

    if (list_empty(&mtx->waiters)) {
        mtx->owner = NULL;
        spin_unlock_irqrestore(&mtx->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return;
    }

    /* Direct handover: transfer ownership to the highest-priority waiter under
     * the lock, preventing third-party barging.
     */
    struct pi_mutex_waiter *w =
        list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
    list_del_init(&w->node);

    /* Update cached top priority after removing the head waiter. */
    if (list_empty(&mtx->waiters))
        mtx->top_waiter_prio = 0;
    else {
        struct pi_mutex_waiter *next_top =
            list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
        mtx->top_waiter_prio = next_top->task->td_prio;
    }

    mtx->owner = w->task;
    w->granted = true;
    /* Link the mutex into the new owner's pi_held_mutexes under mtx->lock
     * so pi_recompute_prio on the new owner sees remaining waiters
     * immediately, with no window between wake and the lock path resuming.
     */
    list_add(&w->task->pi_held_mutexes, &mtx->pi_held);
    pi_recompute_prio(w->task);
    sched_wake_ready(w->task);

    spin_unlock_irqrestore(&mtx->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

void pi_mutex_release_all(struct sched_task *task)
{
    assert(task);

    while (!list_empty(&task->pi_held_mutexes)) {
        struct pi_mutex *mtx =
            list_entry(task->pi_held_mutexes.next, struct pi_mutex, pi_held);

        lockdep_acquire(LOCK_LEVEL_WAITQ);
        u64 flags = spin_lock_irqsave(&mtx->lock);

        if (mtx->owner != task) {
            list_del_init(&mtx->pi_held);
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            continue;
        }

        list_del_init(&mtx->pi_held);

        if (list_empty(&mtx->waiters)) {
            mtx->owner = NULL;
            mtx->top_waiter_prio = 0;
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            continue;
        }

        /* Skip waiters that are already terminating — waking them would
         * revive a task that should stay dead.
         */
        struct pi_mutex_waiter *w = NULL;
        while (!list_empty(&mtx->waiters)) {
            struct pi_mutex_waiter *candidate =
                list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
            list_del_init(&candidate->node);
            if (candidate->task->state != TD_STATE_TERMINATING) {
                w = candidate;
                break;
            }
        }

        if (!w) {
            mtx->owner = NULL;
            mtx->top_waiter_prio = 0;
            spin_unlock_irqrestore(&mtx->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            continue;
        }

        if (list_empty(&mtx->waiters))
            mtx->top_waiter_prio = 0;
        else {
            struct pi_mutex_waiter *next_top =
                list_entry(mtx->waiters.next, struct pi_mutex_waiter, node);
            mtx->top_waiter_prio = next_top->task->td_prio;
        }

        mtx->owner = w->task;
        w->granted = true;
        list_add(&w->task->pi_held_mutexes, &mtx->pi_held);
        pi_recompute_prio(w->task);
        sched_wake_ready(w->task);

        spin_unlock_irqrestore(&mtx->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
    }
}

#include __INC_TEST(mutex)
