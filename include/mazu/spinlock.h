/* SPDX-License-Identifier: MIT */
/* Ticket-less test-and-set spinlock with RISC-V AMO ordering.
 *
 * On SMP: amoswap.w.aq for acquire, amoswap.w.rl for release.
 * On UP:  all operations compile to nothing (zero overhead).
 *
 * irqsave variants combine interrupt disable with the spinlock for
 * contexts that may be entered from both thread and interrupt code.
 */

#ifndef MAZU_SPINLOCK_H
#define MAZU_SPINLOCK_H

#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/critical.h>

#if CONFIG_SMP

typedef struct {
    volatile u32 lock;
} spinlock_t;

#define SPINLOCK_INITIALIZER {.lock = 0}

static inline void spin_lock(spinlock_t *sl)
{
    u32 tmp;
    __asm__ volatile(
        "1:\n"
        "    amoswap.w.aq %0, %2, (%1)\n"
        "    bnez %0, 1b\n"
        : "=&r"(tmp)
        : "r"(&sl->lock), "r"((u32) 1)
        : "memory");
}

static inline void spin_unlock(spinlock_t *sl)
{
    __asm__ volatile("amoswap.w.rl zero, zero, (%0)\n"
                     :
                     : "r"(&sl->lock)
                     : "memory");
}

static inline bool spin_trylock(spinlock_t *sl)
{
    u32 old;
    __asm__ volatile("amoswap.w.aq %0, %2, (%1)\n"
                     : "=r"(old)
                     : "r"(&sl->lock), "r"((u32) 1)
                     : "memory");
    return old == 0;
}

/* Lock with interrupts disabled.  Returns the saved interrupt state. */
static inline u64 spin_lock_irqsave(spinlock_t *sl)
{
    u64 flags = intr_disable();
    spin_lock(sl);
    return flags;
}

/* Unlock and restore interrupt state. */
static inline void spin_unlock_irqrestore(spinlock_t *sl, u64 flags)
{
    spin_unlock(sl);
    intr_restore(flags);
}

#else /* !CONFIG_SMP - UP stubs */

typedef struct {
    char _unused; /* placeholder so the struct is not zero-size */
} spinlock_t;

#define SPINLOCK_INITIALIZER {0}

static inline void spin_lock(spinlock_t *sl __unused) {}
static inline void spin_unlock(spinlock_t *sl __unused) {}
static inline bool spin_trylock(spinlock_t *sl __unused)
{
    return true;
}

static inline u64 spin_lock_irqsave(spinlock_t *sl __unused)
{
    return intr_disable();
}

static inline void spin_unlock_irqrestore(spinlock_t *sl __unused, u64 flags)
{
    intr_restore(flags);
}

#endif /* CONFIG_SMP */

#endif /* MAZU_SPINLOCK_H */
