/* SPDX-License-Identifier: MIT */
/* POSIX reader-writer lock implementation.
 *
 * Writer-preference policy: when a writer is pending, new rdlock requests
 * block behind the writer.  This bounds writer starvation to one reader
 * batch.
 *
 * reader_count > 0: active readers.  reader_count == -1: writer holds.
 * reader_count == 0 && writer_pending: next grant goes to a writer.
 */

#include "rwlock.h"
#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h> /* needed by tests-common.h selftest helpers */
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include "../lockdep.h"
#include "../proc/signal.h"

struct rw_waiter {
    struct sched_task *task;
    struct list_head node;
    bool granted;
};

struct rw_cleanup_ctx {
    struct rwlock *rw;
    struct rw_waiter *waiter;
    struct list_head *waitq;
    struct callout *timeout_callout;
};

static void wake_readers_locked(struct rwlock *rw);

static void rw_wait_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct rw_cleanup_ctx *ctx = ctx_ptr;
    assert(task);
    assert(ctx);

    if (ctx->timeout_callout)
        callout_cancel_sync(ctx->timeout_callout);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->rw->lock);
    if (ctx->waiter->node.next != &ctx->waiter->node)
        list_del_init(&ctx->waiter->node);
    if (ctx->waitq == &ctx->rw->writer_waitq &&
        list_empty(&ctx->rw->writer_waitq)) {
        ctx->rw->writer_pending = false;
        if (ctx->rw->reader_count == 0)
            wake_readers_locked(ctx->rw);
    }
    spin_unlock_irqrestore(&ctx->rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

void rwlock_init(struct rwlock *rw)
{
    rw->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    rw->reader_count = 0;
    rw->writer_owner = NULL;
    list_init(&rw->reader_waitq);
    list_init(&rw->writer_waitq);
    rw->writer_pending = false;
    rw->in_use = true;
}

/* Wake all queued readers (call under rw->lock).
 * Skip TERMINATING waiters: they will never unlock, so granting them
 * would permanently inflate reader_count.
 */
static void wake_readers_locked(struct rwlock *rw)
{
    while (!list_empty(&rw->reader_waitq)) {
        struct rw_waiter *w =
            list_entry(rw->reader_waitq.next, struct rw_waiter, node);
        list_del_init(&w->node);
        if (w->task->state == TD_STATE_TERMINATING)
            continue; /* dead waiter — discard without granting */
        w->granted = true;
        rw->reader_count++;
        sched_wake_ready(w->task);
    }
}

/* Wake the first live queued writer (call under rw->lock).
 * Skip TERMINATING waiters to prevent granting write ownership to
 * a dead task that will never call rwlock_unlock.
 * Returns true if a writer was granted, false if none remain.
 */
static bool wake_writer_locked(struct rwlock *rw)
{
    while (!list_empty(&rw->writer_waitq)) {
        struct rw_waiter *w =
            list_entry(rw->writer_waitq.next, struct rw_waiter, node);
        list_del_init(&w->node);
        if (w->task->state == TD_STATE_TERMINATING)
            continue; /* dead waiter — skip */
        w->granted = true;
        rw->reader_count = -1;
        rw->writer_owner = w->task;
        rw->writer_pending = !list_empty(&rw->writer_waitq);
        sched_wake_ready(w->task);
        return true;
    }
    /* All writers were dead or queue was empty. */
    rw->writer_pending = false;
    return false;
}

i32 rwlock_rdlock_interruptible(struct rwlock *rw)
{
    DEBUG_ASSERT(!in_interrupt_context());
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);

    /* Grant immediately if no writer holds and no writer is pending. */
    if (rw->reader_count >= 0 && !rw->writer_pending) {
        rw->reader_count++;
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct rw_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    struct rw_cleanup_ctx cleanup = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->reader_waitq,
        .timeout_callout = NULL,
    };
    list_init(&w.node);
    list_add_tail(&rw->reader_waitq, &w.node);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, rw_wait_cleanup, &cleanup);

    while (!w.granted) {
        i32 abort = wait_abort_error_current();
        if (abort < 0) {
            list_del_init(&w.node);
            sched_clear_block_cleanup(w.task);
            if (w.task->state == TD_STATE_BLOCKED)
                sched_set_task_state(w.task, TD_STATE_RUNNING);
            spin_unlock_irqrestore(&rw->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return abort;
        }
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        sched_yield_trap();
        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&rw->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

void rwlock_rdlock(struct rwlock *rw)
{
    i32 rc = rwlock_rdlock_interruptible(rw);
    ALWAYS_ASSERT(rc == 0);
}

i32 rwlock_tryrdlock(struct rwlock *rw)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);
    if (rw->reader_count >= 0 && !rw->writer_pending) {
        rw->reader_count++;
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return -(i32) EBUSY;
}

i32 rwlock_wrlock_interruptible(struct rwlock *rw)
{
    DEBUG_ASSERT(!in_interrupt_context());
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);

    if (rw->reader_count == 0) {
        rw->reader_count = -1;
        rw->writer_owner = sched_current_task();
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct rw_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    struct rw_cleanup_ctx cleanup = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->writer_waitq,
        .timeout_callout = NULL,
    };
    list_init(&w.node);
    list_add_tail(&rw->writer_waitq, &w.node);
    rw->writer_pending = true;
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, rw_wait_cleanup, &cleanup);

    while (!w.granted) {
        i32 abort = wait_abort_error_current();
        if (abort < 0) {
            list_del_init(&w.node);
            if (list_empty(&rw->writer_waitq)) {
                rw->writer_pending = false;
                if (rw->reader_count == 0)
                    wake_readers_locked(rw);
            }
            sched_clear_block_cleanup(w.task);
            if (w.task->state == TD_STATE_BLOCKED)
                sched_set_task_state(w.task, TD_STATE_RUNNING);
            spin_unlock_irqrestore(&rw->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return abort;
        }
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        sched_yield_trap();
        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&rw->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

void rwlock_wrlock(struct rwlock *rw)
{
    i32 rc = rwlock_wrlock_interruptible(rw);
    ALWAYS_ASSERT(rc == 0);
}

i32 rwlock_trywrlock(struct rwlock *rw)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);
    if (rw->reader_count == 0) {
        rw->reader_count = -1;
        rw->writer_owner = sched_current_task();
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return -(i32) EBUSY;
}

void rwlock_unlock(struct rwlock *rw)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);

    if (rw->reader_count == -1) {
        /* Writer unlock — verify ownership in debug builds. */
        DEBUG_ASSERT(rw->writer_owner == sched_current_task());
        rw->reader_count = 0;
        rw->writer_owner = NULL;
        /* Writer-preference: try next queued writer first.
         * If all queued writers are dead (TERMINATING), fall back to
         * waking queued readers so they are not stranded forever.
         */
        if (list_empty(&rw->writer_waitq) || !wake_writer_locked(rw))
            wake_readers_locked(rw);
    } else if (rw->reader_count > 0) {
        /* Reader unlock. */
        rw->reader_count--;
        if (rw->reader_count == 0 && rw->writer_pending) {
            if (!wake_writer_locked(rw))
                wake_readers_locked(rw);
        }
    }

    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

/* Timed variants use a callout to wake the waiter on expiry. */
struct rw_timeout_ctx {
    struct rwlock *rw;
    struct rw_waiter *waiter;
    struct list_head *waitq;
    volatile bool timed_out;
};

static void rw_timeout_fn(void *arg)
{
    struct rw_timeout_ctx *ctx = arg;
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->rw->lock);

    if (ctx->waiter->node.next != &ctx->waiter->node && !ctx->waiter->granted &&
        ctx->waiter->task->state != TD_STATE_TERMINATING) {
        list_del_init(&ctx->waiter->node);
        /* Repair writer_pending if this was a writer waiter.
         * Wake stranded readers if no writers remain.
         */
        if (ctx->waitq == &ctx->rw->writer_waitq &&
            list_empty(&ctx->rw->writer_waitq)) {
            ctx->rw->writer_pending = false;
            if (ctx->rw->reader_count == 0)
                wake_readers_locked(ctx->rw);
        }
        ctx->timed_out = true;
        sched_wake_ready(ctx->waiter->task);
    }

    spin_unlock_irqrestore(&ctx->rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

i32 rwlock_timedrdlock(struct rwlock *rw, struct time_ms timeout)
{
    DEBUG_ASSERT(!in_interrupt_context());
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);

    if (rw->reader_count >= 0 && !rw->writer_pending) {
        rw->reader_count++;
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct rw_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    list_init(&w.node);
    list_add_tail(&rw->reader_waitq, &w.node);

    struct rw_timeout_ctx tctx = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->reader_waitq,
        .timed_out = false,
    };
    struct callout tmo;
    callout_init(&tmo);
    struct rw_cleanup_ctx cleanup = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->reader_waitq,
        .timeout_callout = &tmo,
    };
    callout_set_ticks(&tmo, time_ms_to_ticks(timeout.ms), rw_timeout_fn, &tctx);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, rw_wait_cleanup, &cleanup);

    i32 abort = 0;
    while (!w.granted && !tctx.timed_out) {
        abort = wait_abort_error_current();
        if (abort < 0)
            break;
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        sched_yield_trap();
        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&rw->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);

    bool got = w.granted;
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    callout_cancel_sync(&tmo);
    /* Grant takes priority over interruption: if the lock was granted
     * between the signal check and the loop re-evaluation, we own it.
     */
    if (got)
        return 0;
    if (abort < 0)
        return abort;
    return -(i32) ETIMEDOUT;
}

i32 rwlock_timedwrlock(struct rwlock *rw, struct time_ms timeout)
{
    DEBUG_ASSERT(!in_interrupt_context());
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);

    if (rw->reader_count == 0) {
        rw->reader_count = -1;
        rw->writer_owner = sched_current_task();
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return 0;
    }

    struct rw_waiter w = {
        .task = sched_current_task(),
        .granted = false,
    };
    list_init(&w.node);
    list_add_tail(&rw->writer_waitq, &w.node);
    rw->writer_pending = true;

    struct rw_timeout_ctx tctx = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->writer_waitq,
        .timed_out = false,
    };
    struct callout tmo;
    callout_init(&tmo);
    struct rw_cleanup_ctx cleanup = {
        .rw = rw,
        .waiter = &w,
        .waitq = &rw->writer_waitq,
        .timeout_callout = &tmo,
    };
    callout_set_ticks(&tmo, time_ms_to_ticks(timeout.ms), rw_timeout_fn, &tctx);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, rw_wait_cleanup, &cleanup);

    i32 abort = 0;
    while (!w.granted && !tctx.timed_out) {
        abort = wait_abort_error_current();
        if (abort < 0)
            break;
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        sched_yield_trap();
        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&rw->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);

    bool got = w.granted;
    if (!got && list_empty(&rw->writer_waitq)) {
        rw->writer_pending = false;
        if (rw->reader_count == 0)
            wake_readers_locked(rw);
    }
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    callout_cancel_sync(&tmo);
    if (got)
        return 0;
    if (abort < 0)
        return abort;
    return -(i32) ETIMEDOUT;
}

i32 rwlock_destroy(struct rwlock *rw)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&rw->lock);
    if (rw->reader_count != 0 || !list_empty(&rw->reader_waitq) ||
        !list_empty(&rw->writer_waitq)) {
        spin_unlock_irqrestore(&rw->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i32) EBUSY;
    }
    rw->in_use = false;
    spin_unlock_irqrestore(&rw->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

#include __INC_TEST(rwlock)
