/* SPDX-License-Identifier: MIT */
/* Wait queues: universal blocking primitive.
 *
 * Lock ordering: wq->lock (irqsave) -> pcpu_runq_lock (via
 * sched_enqueue_ready). All wq->lock acquisitions use spin_lock_irqsave to
 * prevent ISR deadlock when wake_up is called from callout callbacks or
 * interrupt context.
 */

#include "waitqueue.h"
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/pcpu.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include "../lockdep.h"

static void waitqueue_block_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct wait_queue_entry *e = ctx_ptr;
    assert(task);
    assert(e);
    assert(e->cleanup_wq);

    /* Cancel any stack-local timeout callout BEFORE touching the wait queue.
     * The callout callback may reference the same wqe (e.g.
     * condvar_timeout_fn), so cancel_sync guarantees no in-flight callback
     * after this returns. This must happen before the task stack is freed.
     */
    if (e->timeout_callout)
        callout_cancel_sync(e->timeout_callout);

    u64 flags = intr_disable();
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    spin_lock(&e->cleanup_wq->lock);
    if (e->node.next != &e->node)
        list_del_init(&e->node);
    e->reason = WAIT_UNBLOCK_DESTROY;
    spin_unlock(&e->cleanup_wq->lock);
    lockdep_release(LOCK_LEVEL_WAITQ);
    intr_restore(flags);
}

void init_waitqueue_head(struct wait_queue_head *wq)
{
    assert(wq);
    wq->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    list_init(&wq->head);
}

/* Add task to the wait queue and set it to BLOCKED.
 * Called once before the wait loop begins.
 */
void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    assert(wq);
    assert(e);
    if (!e->task)
        e->task = sched_current_task();
    assert(e->task);
    DEBUG_ASSERT(!in_interrupt_context());

    u64 flags = intr_disable();
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    spin_lock(&wq->lock);

    /* Only add if not already on this queue (idempotent).
     * Keep waiters in descending priority order so wake-up is O(n).
     */
    if (e->node.next == &e->node) {
        struct list_head *pos = &wq->head;
        struct wait_queue_entry *it;
        list_for_each_entry_safe (&wq->head, it, struct wait_queue_entry,
                                  node) {
            if (it->task->td_prio < e->task->td_prio) {
                pos = &it->node;
                break;
            }
        }
        list_add_tail(pos, &e->node);
    }

    /* Do not overwrite a terminal reason if one was delivered asynchronously.
     */
    if (!wait_unblock_is_terminal(e->reason))
        e->reason = WAIT_UNBLOCK_NONE;
    sched_set_task_state(e->task, TD_STATE_BLOCKED);
    e->cleanup_wq = wq;
    e->cleanup_self = e;
    sched_set_block_cleanup(e->task, waitqueue_block_cleanup, e);

    spin_unlock(&wq->lock);
    lockdep_release(LOCK_LEVEL_WAITQ);
    intr_restore(flags);
}

/* Remove task from the wait queue after the condition is met.
 *
 * If the task broke out of wait_event before yielding, prepare_to_wait
 * already set BLOCKED - restore to RUNNING (the task is executing).
 * If wake_up already ran, state is READY or RUNNING; no change needed.
 * Both the check and the write happen under wq->lock to serialize with
 * concurrent wake_up on another hart.
 */
void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e)
{
    assert(wq);
    assert(e);

    u64 flags = intr_disable();
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    spin_lock(&wq->lock);
    list_del_init(&e->node);
    sched_clear_block_cleanup(e->task);
    e->cleanup_wq = NULL;
    e->cleanup_self = NULL;
    if (e->reason == WAIT_UNBLOCK_NONE)
        e->reason = WAIT_UNBLOCK_CONDITION;
    if (e->task->state == TD_STATE_BLOCKED)
        sched_set_task_state(e->task, TD_STATE_RUNNING);
    spin_unlock(&wq->lock);
    lockdep_release(LOCK_LEVEL_WAITQ);
    intr_restore(flags);
}

/* Wake up waiters in queue order.
 * prepare_to_wait inserts by descending priority, so this is O(n).
 * nr=0 means wake all, nr=1 means wake one.
 * Returns the number of tasks transitioned to READY.
 */
static int wake_up_reason(struct wait_queue_head *wq,
                          int nr,
                          enum wait_unblock_reason reason)
{
    assert(wq);
    assert(reason != WAIT_UNBLOCK_NONE);

    int woken = 0;
    u64 wq_flags = intr_disable();
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    spin_lock(&wq->lock);
    struct wait_queue_entry *e;

    list_for_each_entry_safe (&wq->head, e, struct wait_queue_entry, node) {
        if (nr > 0 && woken >= nr)
            break;
        if (e->task->state != TD_STATE_BLOCKED) {
            if (wait_unblock_is_terminal(reason) &&
                !wait_unblock_is_terminal(e->reason))
                e->reason = reason;
            continue;
        }

        e->reason = reason;

        /* Reset watchdog timestamp so time spent blocked does not
         * cause a false-positive hung detection after wake.
         */
        /* wq->lock is held; sched_wake_ready takes pcpu_runq_lock internally
         * (lock ordering: wq->lock -> pcpu_runq_lock).
         */
        sched_wake_ready(e->task);

        woken++;
    }

    spin_unlock(&wq->lock);
    lockdep_release(LOCK_LEVEL_WAITQ);
    intr_restore(wq_flags);
    return woken;
}

int wake_up(struct wait_queue_head *wq, int nr)
{
    return wake_up_reason(wq, nr, WAIT_UNBLOCK_WAKEUP);
}

int wake_up_cancel(struct wait_queue_head *wq, int nr)
{
    return wake_up_reason(wq, nr, WAIT_UNBLOCK_CANCEL);
}

void wait_timeout_fn(void *arg)
{
    struct wait_timeout_arg *a = arg;
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&a->wq->lock);

    if (a->wqe->node.next == &a->wqe->node ||
        a->wqe->task->state != TD_STATE_BLOCKED) {
        spin_unlock_irqrestore(&a->wq->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return;
    }

    a->wqe->reason = WAIT_UNBLOCK_TIMEOUT;
    sched_wake_ready(a->wqe->task);
    spin_unlock_irqrestore(&a->wq->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

/* Internal helpers for the wait_event macro. */
struct sched_task *__wq_current_task(void)
{
    return get_pcpu()->curthread;
}

/* Self-tests */

#include __INC_TEST(waitqueue)
