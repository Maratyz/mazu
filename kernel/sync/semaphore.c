/* SPDX-License-Identifier: MIT */
/* Counting semaphore with direct-handover wake policy.
 *
 * When sem_post() finds a blocked waiter, ownership is transferred
 * directly - the counter never increments, ensuring the waiter runs
 * immediately upon scheduling rather than racing with other posters.
 *
 * Waiters use TD_STATE_SEM_WAIT to distinguish from generic BLOCKED
 * (useful for scheduler telemetry and debugging).
 */

#include "semaphore.h"
#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include "../lockdep.h"
#include "../proc/signal.h"

struct sem_waiter {
    struct sched_task *task;
    struct list_head node;
    bool granted;
};

struct sem_cleanup_ctx {
    struct semaphore *sem;
    struct sem_waiter *waiter;
    struct callout *timeout_callout;
};

static void sem_wait_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct sem_cleanup_ctx *ctx = ctx_ptr;
    assert(task);
    assert(ctx);

    if (ctx->timeout_callout)
        callout_cancel_sync(ctx->timeout_callout);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->sem->lock);
    if (ctx->waiter->node.next != &ctx->waiter->node) {
        /* Still queued: sem_post hasn't reached us yet.  The blocking path
         * never decremented count, so just unlink without restoring.
         */
        list_del_init(&ctx->waiter->node);
    } else if (ctx->waiter->granted) {
        /* Already dequeued by sem_post direct handoff: the token was consumed
         * (count stayed at 0) but we'll never resume sem_wait to use it.
         * Hand it to the next queued waiter if any, otherwise restore count.
         */
        /* Find the first non-terminating waiter to hand the token to.
         * Skip any waiters that are already dying — waking them would
         * revive a task that should stay dead.
         */
        struct sem_waiter *next = NULL;
        while (!list_empty(&ctx->sem->waiters)) {
            struct sem_waiter *candidate =
                list_entry(ctx->sem->waiters.next, struct sem_waiter, node);
            list_del_init(&candidate->node);
            if (candidate->task->state != TD_STATE_TERMINATING) {
                next = candidate;
                break;
            }
        }
        if (next) {
            next->granted = true;
            sched_wake_ready(next->task);
        } else {
            ctx->sem->count++;
        }
    }
    spin_unlock_irqrestore(&ctx->sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

void sem_init(struct semaphore *sem, i32 initial_count)
{
    ALWAYS_ASSERT(initial_count >= 0);
    sem->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    sem->count = initial_count;
    list_init(&sem->waiters);
}

void sem_wait(struct semaphore *sem)
{
    DEBUG_ASSERT(!in_interrupt_context());

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&sem->lock);

    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return;
    }

    /* No tokens available: block. */
    struct sem_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    struct sem_cleanup_ctx cleanup = {
        .sem = sem,
        .waiter = &w,
        .timeout_callout = NULL,
    };
    list_init(&w.node);

    list_add_tail(&sem->waiters, &w.node);
    sched_set_task_state(w.task, TD_STATE_SEM_WAIT);
    sched_set_block_cleanup(w.task, sem_wait_cleanup, &cleanup);
    while (!w.granted) {
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        sched_yield_trap();

        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&sem->lock);
    }

    /* Remove from the wait list if a wake-before-yield race left us linked. */
    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_SEM_WAIT)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

/* Like sem_wait, but returns -EINTR if a signal becomes pending. */
i32 sem_wait_interruptible(struct semaphore *sem)
{
    DEBUG_ASSERT(!in_interrupt_context());

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&sem->lock);

    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct sem_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    struct sem_cleanup_ctx cleanup = {
        .sem = sem,
        .waiter = &w,
        .timeout_callout = NULL,
    };
    list_init(&w.node);

    list_add_tail(&sem->waiters, &w.node);
    sched_set_task_state(w.task, TD_STATE_SEM_WAIT);
    sched_set_block_cleanup(w.task, sem_wait_cleanup, &cleanup);
    while (!w.granted) {
        i32 abort = wait_abort_error_current();
        if (abort < 0) {
            list_del_init(&w.node);
            sched_clear_block_cleanup(w.task);
            if (w.task->state == TD_STATE_SEM_WAIT)
                sched_set_task_state(w.task, TD_STATE_RUNNING);
            spin_unlock_irqrestore(&sem->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return abort;
        }
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        sched_yield_trap();

        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&sem->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_SEM_WAIT)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

i32 sem_trywait(struct semaphore *sem)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&sem->lock);

    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    spin_unlock_irqrestore(&sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return -(i32) EAGAIN;
}

void sem_post(struct semaphore *sem)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&sem->lock);

    if (list_empty(&sem->waiters)) {
        /* No waiters: increment counter. */
        sem->count++;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return;
    }

    /* Direct handover: wake the first waiter, counter stays at 0. */
    struct sem_waiter *w =
        list_entry(sem->waiters.next, struct sem_waiter, node);
    list_del_init(&w->node);
    w->granted = true;

    sched_wake_ready(w->task);

    spin_unlock_irqrestore(&sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

/* Timeout callback for sem_timedwait.  Runs in ISR context.
 * Wakes the waiting task with an indication that the timeout fired.
 */
struct sem_timeout_ctx {
    struct semaphore *sem;
    struct sem_waiter *waiter;
    volatile bool timed_out;
};

static void sem_timeout_fn(void *arg)
{
    struct sem_timeout_ctx *ctx = arg;
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->sem->lock);

    /* Only act if the waiter is still queued and the task is not dying. */
    if (ctx->waiter->node.next != &ctx->waiter->node && !ctx->waiter->granted &&
        ctx->waiter->task->state != TD_STATE_TERMINATING) {
        list_del_init(&ctx->waiter->node);
        ctx->timed_out = true;
        sched_wake_ready(ctx->waiter->task);
    }

    spin_unlock_irqrestore(&ctx->sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

i32 sem_timedwait(struct semaphore *sem, struct time_ms timeout)
{
    DEBUG_ASSERT(!in_interrupt_context());

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&sem->lock);

    if (sem->count > 0) {
        sem->count--;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct sem_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    list_init(&w.node);
    list_add_tail(&sem->waiters, &w.node);

    struct sem_timeout_ctx tctx = {
        .sem = sem,
        .waiter = &w,
        .timed_out = false,
    };
    struct callout tmo;
    callout_init(&tmo);
    struct sem_cleanup_ctx cleanup = {
        .sem = sem,
        .waiter = &w,
        .timeout_callout = &tmo,
    };
    u64 ticks = time_ms_to_ticks(timeout.ms);
    callout_set_ticks(&tmo, ticks, sem_timeout_fn, &tctx);

    sched_set_task_state(w.task, TD_STATE_SEM_WAIT);
    sched_set_block_cleanup(w.task, sem_wait_cleanup, &cleanup);

    i32 abort = 0;
    while (!w.granted && !tctx.timed_out) {
        abort = wait_abort_error_current();
        if (abort < 0)
            break;
        spin_unlock_irqrestore(&sem->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        sched_yield_trap();

        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&sem->lock);
    }

    /* Remove from wait list while still under sem->lock. */
    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_SEM_WAIT)
        sched_set_task_state(w.task, TD_STATE_RUNNING);

    bool got_token = w.granted;
    spin_unlock_irqrestore(&sem->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    /* Synchronous cancel after dropping sem->lock to avoid deadlock
     * (the callback acquires sem->lock).  This ensures the ISR callback
     * has fully returned before we unwind the stack-allocated tmo/tctx.
     */
    callout_cancel_sync(&tmo);

    if (got_token)
        return 0;
    return (abort < 0) ? abort : -(i32) ETIMEDOUT;
}

#include __INC_TEST(semaphore)
