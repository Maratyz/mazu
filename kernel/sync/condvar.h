/* SPDX-License-Identifier: MIT */
/* Condition variable with timed wait support.
 *
 * Paired with pi_mutex: condvar_wait atomically releases the mutex and
 * blocks, then reacquires the mutex on wakeup.  Timed wait uses the
 * callout engine and returns ETIMEDOUT on expiry.
 *
 * Lock ordering: cv->wq.lock (WAITQ) -> pcpu_runq_lock (SCHED).
 */

#ifndef MAZU_CONDVAR_H
#define MAZU_CONDVAR_H

#include <mazu/time.h>
#include "../sched/waitqueue.h"
#include "mutex.h"

struct condvar {
    struct wait_queue_head wq;
};

void condvar_init(struct condvar *cv);

/* Wait on the condition variable, releasing mtx atomically.
 * Reacquires mtx before returning.
 */
i32 condvar_wait(struct condvar *cv, struct pi_mutex *mtx);

/* Wait with timeout.  Returns 0 on signal, -ETIMEDOUT on expiry.
 * Reacquires mtx before returning regardless.
 */
i32 condvar_wait_timeout(struct condvar *cv,
                         struct pi_mutex *mtx,
                         struct time_ms timeout);

/* Wake one waiter. */
void condvar_signal(struct condvar *cv);

/* Wake all waiters. */
void condvar_broadcast(struct condvar *cv);

#endif /* MAZU_CONDVAR_H */
