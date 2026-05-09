/* SPDX-License-Identifier: MIT */
#include <mazu/critical.h>
#include <mazu/pcpu.h>
#include <mazu/selftest.h>
#include <mazu/spinlock.h>
#include <sbi.h>

static i32 test_sbi_probe(void)
{
    /* The Base extension (0x10) must always be present. */
    return sbi_probe_extension(SBI_EXT_BASE) ? 0 : 1;
}
DEFINE_SELFTEST(sbi_probe, test_sbi_probe);

static i32 test_spinlock_basic(void)
{
    spinlock_t sl = SPINLOCK_INITIALIZER;

#if CONFIG_SMP
    /* Lock, verify trylock fails, unlock, verify trylock succeeds. */
    spin_lock(&sl);
    bool got = spin_trylock(&sl);
    spin_unlock(&sl);
    if (got)
        return 1; /* trylock should have failed while held */

    got = spin_trylock(&sl);
    if (!got)
        return 2; /* trylock should succeed after unlock */
    spin_unlock(&sl);
#else
    /* UP spinlocks are intentional stubs. They only guard interrupt state via
     * the irqsave helpers, so plain trylock always succeeds.
     */
    spin_lock(&sl);
    if (!spin_trylock(&sl))
        return 1;
    spin_unlock(&sl);
#endif

    return 0;
}
DEFINE_SELFTEST(spinlock_basic, test_spinlock_basic);

static i32 test_critical_nesting(void)
{
    /* Save current interrupt state. */
    u64 sstatus_before;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_before));
    bool was_enabled = (sstatus_before & 2) != 0;

    /* Enable interrupts so the test can verify disable/restore. */
    __asm__ volatile("csrsi sstatus, 2" : : : "memory");

    critical_enter();
    /* Interrupts should be disabled. */
    u64 sstatus_mid;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_mid));
    if (sstatus_mid & 2)
        return 1; /* interrupts still enabled inside critical section */

    critical_enter(); /* nested */

    critical_exit(); /* back to depth 1 */
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_mid));
    if (sstatus_mid & 2)
        return 2; /* interrupts restored too early (still nested) */

    critical_exit(); /* depth 0 - should restore */
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus_mid));
    if (!(sstatus_mid & 2))
        return 3; /* interrupts not restored after final exit */

    /* Restore original state. */
    if (!was_enabled)
        __asm__ volatile("csrci sstatus, 2" : : : "memory");

    return 0;
}
DEFINE_SELFTEST(critical_nesting, test_critical_nesting);

static i32 test_pcpu_bsp(void)
{
    struct pcpu *pc = get_pcpu();
    if (!pc)
        return 1;
    /* BSP should be logical cpu 0 (hartid may vary). */
    if (pc->cpuid != 0)
        return 2;
    return 0;
}
DEFINE_SELFTEST(pcpu_bsp, test_pcpu_bsp);
