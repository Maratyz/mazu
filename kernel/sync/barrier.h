/* SPDX-License-Identifier: MIT */
/* POSIX barrier (pthread_barrier).
 *
 * Generation-based design: waiters sleep on "generation changed", not
 * "count reached zero".  This prevents a fast thread from entering the
 * next barrier round before laggards from the previous round have left.
 *
 * Lock ordering: barrier->lock (WAITQ) -> pcpu_runq_lock (SCHED).
 */

#ifndef MAZU_SYNC_BARRIER_H
#define MAZU_SYNC_BARRIER_H

#include <mazu/list.h>
#include <mazu/spinlock.h>

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

struct barrier {
    spinlock_t lock;
    u32 trip_count;     /* number of threads required to trip */
    u32 count;          /* threads still needed for current generation */
    u32 generation;     /* incremented each time the barrier trips */
    u32 active_waiters; /* threads inside barrier_wait() (not just queued) */
    struct list_head waiters; /* list of barrier_waiter, FIFO */
    bool in_use;
};

/* Initialize a barrier for 'count' threads. */
void barrier_init(struct barrier *b, u32 count);

/* Wait at the barrier.  Blocks until 'count' threads have arrived.
 * Returns PTHREAD_BARRIER_SERIAL_THREAD for exactly one waiter, 0 for rest.
 */
i32 barrier_wait(struct barrier *b);
i32 barrier_wait_interruptible(struct barrier *b);

/* Destroy the barrier.  Returns -EBUSY if threads are blocked. */
i32 barrier_destroy(struct barrier *b);

#endif /* MAZU_SYNC_BARRIER_H */
