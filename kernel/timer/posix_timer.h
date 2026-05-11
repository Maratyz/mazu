/* SPDX-License-Identifier: MIT */
/* POSIX per-task interval timers for PSE51.
 *
 * Backed by the callout engine. Timer expiry delivers SIGALRM
 * to the owning process via the signal infrastructure.
 */

#ifndef MAZU_POSIX_TIMER_H
#define MAZU_POSIX_TIMER_H

#include <mazu/base.h>
#include <mazu/callout.h>

struct proc;

#define POSIX_TIMER_MAX 8

struct posix_timer {
    struct callout co;
    struct proc *owner;
    u32 owner_generation;
    u64 interval_ticks; /* 0 = one-shot */
    u32 overrun;
    bool armed;
    bool in_use;
    /* SIGEV_THREAD_ID target TID. 0 selects process-directed delivery
     * (the kernel's signal_send picks an eligible thread). Non-zero
     * directs SIGALRM at the matching thread of the owning proc; if
     * that thread no longer exists at expiry, fall back to process-
     * directed delivery so the bit is not silently dropped.
     */
    u16 target_tid;
};

/* Allocate a timer for the given process.  Returns handle >= 0 or -EAGAIN. */
i32 posix_timer_create(struct proc *p);

/* Arm a timer.  Caller must be the owner.
 * value_ms = initial expiry (0 = disarm), interval_ms = repeat (0 = one-shot).
 * target_tid = SIGEV_THREAD_ID target (0 = process-directed).
 */
i32 posix_timer_settime(i32 handle,
                        struct proc *caller,
                        u64 value_ms,
                        u64 interval_ms,
                        u16 target_tid);

/* Disarm and delete a timer.  Caller must be the owner. */
i32 posix_timer_delete(i32 handle, struct proc *caller);

/* Get remaining time in ms.  Caller must be the owner.
 * Returns ms >= 0 or -EINVAL/-EPERM.
 */
i64 posix_timer_gettime(i32 handle, struct proc *caller);

/* Get overrun count.  Caller must be the owner.
 * Returns count >= 0 or -EINVAL/-EPERM.
 */
i64 posix_timer_getoverrun(i32 handle, struct proc *caller);

/* Delete all timers owned by a process.  Called during process teardown. */
void posix_timer_teardown_proc(struct proc *p);

#endif /* MAZU_POSIX_TIMER_H */
