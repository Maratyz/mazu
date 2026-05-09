/* SPDX-License-Identifier: MIT */
/* Nestable interrupt-disable critical sections.
 *
 * critical_enter() disables supervisor interrupts and increments a per-hart
 * nesting counter.  critical_exit() decrements the counter and restores
 * interrupts only when the outermost section exits.
 *
 * On UP builds the nesting counter is a plain global.  On SMP builds it
 * lives in struct pcpu (accessed via the gp register).
 */

#ifndef MAZU_CRITICAL_H
#define MAZU_CRITICAL_H

#include <mazu/assert.h>
#include <mazu/base.h>

/* Read sstatus.SIE and clear it atomically.  Returns the old SIE value
 * (bit 1 of sstatus) - nonzero means interrupts were enabled.
 */
static inline u64 intr_disable(void)
{
    u64 old;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(old) : : "memory");
    return old & 2;
}

/* Restore sstatus.SIE if 'saved' indicates it was previously set. */
static inline void intr_restore(u64 saved)
{
    if (saved)
        __asm__ volatile("csrsi sstatus, 2" : : : "memory");
}

#if CONFIG_SMP

/* SMP version: nesting state lives in struct pcpu (via gp register).
 * The pcpu fields critnest and saved_sie are at known offsets.
 * Defined after pcpu.h is available; forward-declared here for the
 * inline functions.
 */
#include <mazu/pcpu.h>

static inline void critical_enter(void)
{
    u64 saved = intr_disable();
    struct pcpu *pc = get_pcpu();
    if (pc->critnest == 0)
        pc->saved_sie = saved;
    pc->critnest++;
}

static inline void critical_exit(void)
{
    struct pcpu *pc = get_pcpu();
    DEBUG_ASSERT(pc->critnest > 0);
    pc->critnest--;
    if (pc->critnest == 0)
        intr_restore(pc->saved_sie);
}

#else /* !CONFIG_SMP - UP version */

/* Global nesting counter for the single hart. */
extern u32 _up_critnest;
extern u64 _up_saved_sie;

static inline void critical_enter(void)
{
    u64 saved = intr_disable();
    if (_up_critnest == 0)
        _up_saved_sie = saved;
    _up_critnest++;
}

static inline void critical_exit(void)
{
    DEBUG_ASSERT(_up_critnest > 0);
    _up_critnest--;
    if (_up_critnest == 0)
        intr_restore(_up_saved_sie);
}

#endif /* CONFIG_SMP */

#endif /* MAZU_CRITICAL_H */
