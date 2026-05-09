/* SPDX-License-Identifier: MIT */
/* POSIX reader-writer lock.
 *
 * Writer-preference: once a writer is waiting, new readers queue behind it.
 * This prevents writer starvation under read-heavy workloads.
 *
 * No priority inheritance on the read side (no single owner to boost).
 * Document: rwlocks are unsuitable for hard-RT critical paths; use pi_mutex.
 *
 * Lock ordering: rwlock->lock (WAITQ) -> pcpu_runq_lock (SCHED).
 */

#ifndef MAZU_RWLOCK_H
#define MAZU_RWLOCK_H

#include <mazu/list.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>

struct rwlock {
    spinlock_t lock;
    i32 reader_count;                /* active readers (>0) or writer (-1) */
    struct sched_task *writer_owner; /* current writer, or NULL */
    struct list_head reader_waitq;   /* blocked readers */
    struct list_head writer_waitq;   /* blocked writers */
    bool writer_pending;             /* a writer is waiting (preference flag) */
    bool in_use;
};

void rwlock_init(struct rwlock *rw);

/* Acquire shared read lock.  Blocks if a writer holds or is waiting. */
void rwlock_rdlock(struct rwlock *rw);
i32 rwlock_rdlock_interruptible(struct rwlock *rw);

/* Try shared read lock.  Returns 0 on success, -EBUSY if would block. */
i32 rwlock_tryrdlock(struct rwlock *rw);

/* Acquire exclusive write lock.  Blocks until all readers and writers release.
 */
void rwlock_wrlock(struct rwlock *rw);
i32 rwlock_wrlock_interruptible(struct rwlock *rw);

/* Try exclusive write lock.  Returns 0 on success, -EBUSY if would block. */
i32 rwlock_trywrlock(struct rwlock *rw);

/* Release read or write lock. */
void rwlock_unlock(struct rwlock *rw);

/* Timed read lock.  Returns 0 on success, -ETIMEDOUT on expiry. */
i32 rwlock_timedrdlock(struct rwlock *rw, struct time_ms timeout);

/* Timed write lock.  Returns 0 on success, -ETIMEDOUT on expiry. */
i32 rwlock_timedwrlock(struct rwlock *rw, struct time_ms timeout);

/* Destroy.  Returns -EBUSY if held. */
i32 rwlock_destroy(struct rwlock *rw);

#endif /* MAZU_RWLOCK_H */
