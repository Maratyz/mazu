/* SPDX-License-Identifier: MIT */
/* Futex: fast userspace mutual exclusion.
 *
 * Hash-table of wait queues keyed by user virtual address.
 * FUTEX_WAIT atomically checks the user word and blocks;
 * FUTEX_WAKE wakes waiters on the same address.
 *
 * Lock ordering: futex bucket lock (WAITQ) -> pcpu_runq_lock (SCHED).
 */

#ifndef MAZU_FUTEX_H
#define MAZU_FUTEX_H

#include <mazu/base.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMP_REQUEUE 2
#define FUTEX_LOCK_PI 3
#define FUTEX_UNLOCK_PI 4

/* Initialize the futex hash table.  Called once at boot. */
void futex_init(void);

/* FUTEX_WAIT: if *uaddr == expected, block until woken.
 * Returns 0 on wake, -EAGAIN if value mismatch, -EFAULT on bad pointer.
 */
i64 futex_wait(ptr uaddr, u32 expected);

/* FUTEX_WAKE: wake up to max_wake waiters blocked on uaddr.
 * Returns number of tasks woken.
 */
i64 futex_wake(ptr uaddr, u32 max_wake);

/* FUTEX_CMP_REQUEUE: atomically wake up to nr_wake waiters on uaddr1,
 * then move up to nr_requeue remaining waiters from uaddr1 to uaddr2.
 * The operation aborts with -EAGAIN if *uaddr1 != expected.
 * Returns total number of woken + requeued tasks.
 */
i64 futex_cmp_requeue(ptr uaddr1,
                      u32 expected,
                      ptr uaddr2,
                      u32 nr_wake,
                      u32 nr_requeue);

/* FUTEX_LOCK_PI: acquire PI-aware futex.  Blocks with priority
 * inheritance if the futex is held by another task.
 * Returns 0 on acquisition, -EFAULT on bad pointer.
 */
i64 futex_lock_pi(ptr uaddr);

/* FUTEX_UNLOCK_PI: release PI-aware futex and wake highest-priority waiter.
 * Returns 0 on success, -EFAULT on bad pointer, -EPERM if not owner.
 */
i64 futex_unlock_pi(ptr uaddr);

/* Walk the robust futex list of a dying process and unlock orphaned
 * futexes.  Called from the process exit path.  Writes 0 to each
 * futex word and wakes one waiter per entry.
 */
struct proc;
struct sched_task;
void futex_exit_robust_list(struct proc *p);

/* Walk one thread's robust futex list and clear its registration.
 * Used by SYS_THREAD_EXIT for non-last-thread exits, where the
 * dying thread must release its own held futexes without tearing
 * down the rest of the process.
 */
void futex_exit_robust_list_task(struct sched_task *td);

#endif /* MAZU_FUTEX_H */
