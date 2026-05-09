/* SPDX-License-Identifier: MIT */
/* Wait queues: universal blocking primitive for kernel synchronization.
 *
 * Tasks block by entering TD_STATE_BLOCKED via prepare_to_wait, and are
 * woken by wake_up (which transitions them to READY and enqueues them).
 *
 * Lock ordering (global): wq->lock -> pcpu_runq_lock (never reversed).
 */

#ifndef MAZU_WAITQUEUE_H
#define MAZU_WAITQUEUE_H

#include <mazu/list.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>

struct sched_task; /* forward declaration */

struct wait_queue_head {
    spinlock_t lock;
    struct list_head head;
};

enum wait_unblock_reason {
    WAIT_UNBLOCK_NONE = 0,
    WAIT_UNBLOCK_CONDITION = 1,
    WAIT_UNBLOCK_WAKEUP = 2,
    WAIT_UNBLOCK_TIMEOUT = 3,
    WAIT_UNBLOCK_CANCEL = 4,
    WAIT_UNBLOCK_DESTROY = 5,
};

struct wait_queue_entry {
    struct sched_task *task;
    enum wait_unblock_reason reason;
    struct list_head node;
    struct wait_queue_head *cleanup_wq;
    struct wait_queue_entry *cleanup_self;
    struct callout *timeout_callout; /* armed stack-local callout to cancel
                                        on task death; NULL if no timeout */
};

#define WAIT_QUEUE_HEAD_INITIALIZER(name)                                    \
    {                                                                        \
        .lock = SPINLOCK_INITIALIZER, .head = { &(name).head, &(name).head } \
    }

void init_waitqueue_head(struct wait_queue_head *wq);
void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
int wake_up(struct wait_queue_head *wq, int nr);
int wake_up_cancel(struct wait_queue_head *wq, int nr);

static inline bool wait_unblock_is_terminal(enum wait_unblock_reason reason)
{
    return reason == WAIT_UNBLOCK_TIMEOUT || reason == WAIT_UNBLOCK_CANCEL ||
           reason == WAIT_UNBLOCK_DESTROY;
}

static inline bool wait_should_exit(enum wait_unblock_reason reason)
{
    return wait_unblock_is_terminal(reason) || reason == WAIT_UNBLOCK_WAKEUP;
}

struct wait_timeout_arg {
    struct wait_queue_head *wq;
    struct wait_queue_entry *wqe;
};

void wait_timeout_fn(void *arg);

/* Lost-wakeup-safe blocking macro.  prepare_to_wait is called inside the
 * loop so the lock/unlock provides a memory barrier between setting
 * TD_STATE_BLOCKED and re-evaluating the condition - preventing SMP
 * reordering from causing lost wakeups.  This matches the Linux pattern.
 */
#define wait_event_reason(wq, cond, reason_out)                   \
    do {                                                          \
        struct wait_queue_entry __wqe = {0};                      \
        __wqe.task = __wq_current_task();                         \
        __wqe.reason = WAIT_UNBLOCK_NONE;                         \
        list_init(&__wqe.node);                                   \
        while (!(cond)) {                                         \
            prepare_to_wait(&(wq), &__wqe);                       \
            if ((cond) || wait_unblock_is_terminal(__wqe.reason)) \
                break;                                            \
            sched_yield_trap();                                   \
            if (wait_unblock_is_terminal(__wqe.reason))           \
                break;                                            \
        }                                                         \
        finish_wait(&(wq), &__wqe);                               \
        (reason_out) = __wqe.reason;                              \
    } while (0)

#define wait_event(wq, cond)                              \
    do {                                                  \
        enum wait_unblock_reason __reason_unused;         \
        wait_event_reason((wq), (cond), __reason_unused); \
        (void) __reason_unused;                           \
    } while (0)

/* Internal helper for wait_event macro; do not call directly. */
struct sched_task *__wq_current_task(void);

#endif /* MAZU_WAITQUEUE_H */
