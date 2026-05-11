/* SPDX-License-Identifier: MIT */
/* POSIX barrier implementation.
 *
 * Generation-based: on last arrival, increment generation, reset count,
 * select one serial thread, wake all waiters from the old generation.
 * Waiters check generation change, not count == 0.
 */

#include "barrier.h"
#include <mazu/assert.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h> /* needed by tests-common.h selftest helpers */
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include "../lockdep.h"
#include "../proc/signal.h"

struct barrier_waiter {
    struct sched_task *task;
    struct list_head node;
    u32 wait_gen; /* generation this waiter entered */
    i32 result;   /* PTHREAD_BARRIER_SERIAL_THREAD or 0 */
};

struct barrier_cleanup_ctx {
    struct barrier *barrier;
    struct barrier_waiter *waiter;
};

static void barrier_wait_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct barrier_cleanup_ctx *ctx = ctx_ptr;
    assert(task);
    assert(ctx);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->barrier->lock);
    if (ctx->waiter->node.next != &ctx->waiter->node) {
        list_del_init(&ctx->waiter->node);
        ctx->barrier->count++;
        ctx->barrier->active_waiters--;
    }
    spin_unlock_irqrestore(&ctx->barrier->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

void barrier_init(struct barrier *b, u32 count)
{
    ALWAYS_ASSERT(count > 0);
    b->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    b->trip_count = count;
    b->count = count;
    b->generation = 0;
    b->active_waiters = 0;
    list_init(&b->waiters);
    b->in_use = true;
}

i32 barrier_wait_interruptible(struct barrier *b)
{
    DEBUG_ASSERT(!in_interrupt_context());

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);

    b->active_waiters++;
    b->count--;
    if (b->count == 0) {
        /* Last arrival: trip the barrier. */
        b->count = b->trip_count;
        b->generation++;

        /* Wake all waiters from the old generation.
         * The first waiter gets SERIAL_THREAD, rest get 0.
         */
        bool first = true;
        while (!list_empty(&b->waiters)) {
            struct barrier_waiter *w =
                list_entry(b->waiters.next, struct barrier_waiter, node);
            list_del_init(&w->node);
            w->result = first ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
            first = false;
            sched_wake_ready(w->task);
        }

        b->active_waiters--;
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        /* The arriving thread that trips the barrier is NOT the serial thread
         * (the serial thread is the first waiter that was already sleeping).
         * If no one was waiting (count==1 barrier), this thread is serial.
         */
        return first ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
    }

    /* Not the last: block until generation changes. */
    struct barrier_waiter w = {
        .task = sched_current_task(),
        .wait_gen = b->generation,
        .result = 0,
    };
    struct barrier_cleanup_ctx cleanup = {
        .barrier = b,
        .waiter = &w,
    };
    list_init(&w.node);
    list_add_tail(&b->waiters, &w.node);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, barrier_wait_cleanup, &cleanup);

    while (b->generation == w.wait_gen) {
        i32 abort = wait_abort_error_current();
        if (abort < 0) {
            list_del_init(&w.node);
            sched_clear_block_cleanup(w.task);
            if (w.task->state == TD_STATE_BLOCKED)
                sched_set_task_state(w.task, TD_STATE_RUNNING);
            b->count++;
            b->active_waiters--;
            spin_unlock_irqrestore(&b->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return abort;
        }
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);

        sched_yield_trap();

        lockdep_acquire(LOCK_LEVEL_WAITQ);
        flags = spin_lock_irqsave(&b->lock);
    }

    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    b->active_waiters--;

    i32 ret = w.result;
    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return ret;
}

i32 barrier_wait(struct barrier *b)
{
    return barrier_wait_interruptible(b);
}

i32 barrier_destroy(struct barrier *b)
{
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);

    if (!list_empty(&b->waiters) || b->active_waiters > 0) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i32) EBUSY;
    }

    b->in_use = false;
    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

#include __INC_TEST(barrier)
