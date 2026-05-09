/* SPDX-License-Identifier: MIT */
/* Lock ordering enforcement (sotOS inspired).
 *
 * Debug-mode lock hierarchy checker.  Each lock in the kernel is assigned
 * a level.  A per-CPU bitmask tracks which levels are currently held.
 * Acquiring a lock at level N when any lock at level >= N is held triggers
 * an assertion.
 *
 * Zero cost in release builds (gated on __DEBUG__ > 0).
 *
 * Lock hierarchy (lower numbers acquired first):
 *   IRQ(0) < PROC(1) < FD(2) < SIG(3) < WAITQ(4) < TCP(5) < SCHED(6)
 *          < CALLOUT(7) < ALLOC(8)
 *
 * Usage: call lockdep_acquire(level) before spin_lock, lockdep_release(level)
 * after spin_unlock at each annotated lock site.  Unannotated locks use
 * LOCK_LEVEL_NONE and are not checked.
 *
 * Ref: externals/sotOS/kernel/src/sync/lock_order.rs
 */

#ifndef MAZU_LOCKDEP_H
#define MAZU_LOCKDEP_H

#include <mazu/base.h>

enum lock_level {
    LOCK_LEVEL_IRQ = 0,     /* irqdesc table */
    LOCK_LEVEL_PROC = 1,    /* process table */
    LOCK_LEVEL_FD = 2,      /* per-process fd tables */
    LOCK_LEVEL_SIG = 3,     /* per-process signal state */
    LOCK_LEVEL_WAITQ = 4,   /* wait queues */
    LOCK_LEVEL_TCP = 5,     /* TCP connection locks */
    LOCK_LEVEL_SCHED = 6,   /* scheduler run queue */
    LOCK_LEVEL_CALLOUT = 7, /* callout list */
    LOCK_LEVEL_ALLOC = 8,   /* buddy/kvalloc */
    LOCK_LEVEL_NONE = 255,  /* untracked locks (no ordering check) */
};

#if __DEBUG__ > 0

#include <mazu/assert.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>

/* Check that no lock at level >= 'level' is currently held on this CPU.
 * Acquiring a lock at 'level' while holding a same-or-higher level lock
 * violates the lock ordering hierarchy.
 */
static inline void lockdep_acquire(enum lock_level level)
{
    if (level == LOCK_LEVEL_NONE)
        return;
    struct pcpu *pc = get_pcpu();
    u16 violation_mask = (u16) (0xFFFFU << level);
    if (pc->held_locks & violation_mask) {
        printk(KERN_ERR,
               STR("lockdep acquire violation: cpu=%u level=%u held=0x%hx "
                   "mask=0x%hx\n"),
               pc->cpuid, (u32) level, pc->held_locks, violation_mask);
    }
    DEBUG_ASSERT((pc->held_locks & violation_mask) == 0);
    pc->held_locks |= (u16) (1U << level);
}

/* Release a lock level on this CPU.  Asserts the level was actually held
 * to catch double-release or mismatched release bugs.
 */
static inline void lockdep_release(enum lock_level level)
{
    if (level == LOCK_LEVEL_NONE)
        return;
    struct pcpu *pc = get_pcpu();
    if (!(pc->held_locks & (1U << level))) {
        printk(KERN_ERR,
               STR("lockdep release violation: cpu=%u level=%u held=0x%hx\n"),
               pc->cpuid, (u32) level, pc->held_locks);
    }
    DEBUG_ASSERT(pc->held_locks & (1U << level));
    pc->held_locks &= (u16) ~(1U << level);
}

#else /* !__DEBUG__ */

static inline void lockdep_acquire(enum lock_level level __unused) {}
static inline void lockdep_release(enum lock_level level __unused) {}

#endif /* __DEBUG__ */

#endif /* MAZU_LOCKDEP_H */
