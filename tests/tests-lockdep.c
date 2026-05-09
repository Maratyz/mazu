/* SPDX-License-Identifier: MIT */
#include <kernel/lockdep.h>
#include <mazu/asm.h>
#include <mazu/pcpu.h>
#include <mazu/selftest.h>

/* Test correct lock ordering: acquire in ascending order, verify no crash. */
static i32 test_lockdep_correct_order(void)
{
#if __DEBUG__ > 0
    /* Disable interrupts to prevent ISR lockdep_acquire from seeing our
     * artificially inflated held_locks and triggering a false panic.
     */
    disable_interrupts();

    struct pcpu *pc = get_pcpu();
    u16 saved = pc->held_locks;

    /* Acquire in correct ascending order: WAITQ(4) then SCHED(6). */
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    if (!(pc->held_locks & (1U << LOCK_LEVEL_WAITQ)))
        goto fail;

    lockdep_acquire(LOCK_LEVEL_SCHED);
    if (!(pc->held_locks & (1U << LOCK_LEVEL_SCHED)))
        goto fail;

    /* Release in reverse order. */
    lockdep_release(LOCK_LEVEL_SCHED);
    if (pc->held_locks & (1U << LOCK_LEVEL_SCHED))
        goto fail;

    lockdep_release(LOCK_LEVEL_WAITQ);
    if (pc->held_locks & (1U << LOCK_LEVEL_WAITQ))
        goto fail;

    /* held_locks should be restored to saved state. */
    if (pc->held_locks != saved)
        goto fail;

    enable_interrupts();
    return 0;

fail:
    pc->held_locks = saved;
    enable_interrupts();
    return 1;
#else
    return 0;
#endif
}
DEFINE_SELFTEST(lockdep_correct_order, test_lockdep_correct_order);

/* Test LOCK_LEVEL_NONE: acquiring NONE level should be a no-op. */
static i32 test_lockdep_none_level(void)
{
#if __DEBUG__ > 0
    struct pcpu *pc = get_pcpu();
    u16 saved = pc->held_locks;

    /* NONE operations must not touch held_locks at all. */
    lockdep_acquire(LOCK_LEVEL_NONE);
    if (pc->held_locks != saved)
        goto fail;

    lockdep_release(LOCK_LEVEL_NONE);
    if (pc->held_locks != saved)
        goto fail;

    return 0;

fail:
    pc->held_locks = saved;
    return 1;
#else
    return 0;
#endif
}
DEFINE_SELFTEST(lockdep_none_level, test_lockdep_none_level);

/* Test bitmask tracking: acquire multiple levels and verify the mask. */
static i32 test_lockdep_bitmask(void)
{
#if __DEBUG__ > 0
    /* Disable interrupts; same rationale as test_lockdep_correct_order. */
    disable_interrupts();

    struct pcpu *pc = get_pcpu();
    u16 saved = pc->held_locks;

    lockdep_acquire(LOCK_LEVEL_IRQ);
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    lockdep_acquire(LOCK_LEVEL_SCHED);
    lockdep_acquire(LOCK_LEVEL_CALLOUT);
    lockdep_acquire(LOCK_LEVEL_ALLOC);

    u16 expected = (1U << LOCK_LEVEL_IRQ) | (1U << LOCK_LEVEL_WAITQ) |
                   (1U << LOCK_LEVEL_SCHED) | (1U << LOCK_LEVEL_CALLOUT) |
                   (1U << LOCK_LEVEL_ALLOC);

    if ((pc->held_locks & expected) != expected)
        goto fail;

    lockdep_release(LOCK_LEVEL_ALLOC);
    lockdep_release(LOCK_LEVEL_CALLOUT);
    lockdep_release(LOCK_LEVEL_SCHED);
    lockdep_release(LOCK_LEVEL_WAITQ);
    lockdep_release(LOCK_LEVEL_IRQ);

    if (pc->held_locks != saved)
        goto fail;

    enable_interrupts();
    return 0;

fail:
    pc->held_locks = saved;
    enable_interrupts();
    return 1;
#else
    return 0;
#endif
}
DEFINE_SELFTEST(lockdep_bitmask, test_lockdep_bitmask);
