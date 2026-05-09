/* SPDX-License-Identifier: MIT */
/* SMP secondary hart boot.
 *
 * _secondary_start() is the SBI HSM entry point for secondary harts.
 * smp_boot_secondaries() is called from kernel_init() after full init to bring
 * up secondary harts via SBI HSM.
 *
 * Secondaries initialize their per-hart state and park in wfi until the CPU
 * scheduler is SMP-aware and assigns them runnable tasks.
 */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/base.h>
#include <mazu/callout.h>
#include <mazu/cpumask.h>
#include <mazu/eventlog.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/ipi.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/time.h>
#include <sbi.h>

/* Shared satp value written by BSP before starting secondaries.
 * Secondaries load this into their satp register.
 */
static u64 _shared_satp;

struct pcpu pcpu_array[MAX_CPUS] __aligned(64);
char intr_stacks[MAX_CPUS][INTR_STACK_SIZE] __aligned(16);
volatile u32 nr_cpus_online = 1; /* BSP is always online */

/* C portion of secondary init, called from _secondary_start asm. */
static void __used secondary_init_c(struct pcpu *pc)
{
    /* Defensive re-write: sscratch is already set to intr_stack_top in
     * _secondary_start asm, but reaffirm it before installing stvec.
     */
    __asm__ volatile("csrw sscratch, %0"
                     :
                     : "r"(pc->intr_stack_top)
                     : "memory");

    /* Initialize trap vector, PLIC threshold, and interrupt enables for this
     * hart so it can receive device and IPI interrupts.
     */
    extern void interrupt_init_secondary(void);
    interrupt_init_secondary();

    /* Mark online before incrementing the counter. Use .rl on the increment so
     * the online store is visible to the BSP before it sees the new count and
     * calls ipi_send_broadcast().
     */
    pc->online = true;
    mp_set_cpu_active(pc->cpuid);

    __asm__ volatile("amoadd.w.rl zero, %1, (%0)"
                     :
                     : "r"(&nr_cpus_online), "r"((u32) 1)
                     : "memory");

    printk(KERN_INFO, STR("hart %u online (cpu %u)\n"), (unsigned) pc->hartid,
           (unsigned) pc->cpuid);

    /* Initialize this hart's timer and enter the scheduler.
     * sched_enter_secondary() never returns - it runs the idle loop, and timer
     * interrupts drive context switches to real tasks from global run queue.
     */
    time_init_secondary();
    initgraph_run(INIT_FLAG_SECONDARY);
    sched_enter_secondary();
}

/* Naked SBI HSM entry point.
 *
 * Called by OpenSBI with:
 *   a0 = hartid
 *   a1 = opaque (pointer to this hart's struct pcpu)
 *
 * Must set up gp, sscratch, sp, satp, then call secondary_init_c().
 */
__attribute__((naked, section(".text"), noreturn)) void _secondary_start(void)
{
    __asm__(
        /* a0 = hartid, a1 = opaque (pcpu pointer) */

        /* Disable interrupts. */
        "csrci sstatus, 2\n"

        /* Set gp = pcpu pointer (a1). */
        "mv    gp, a1\n"

        /* Set sscratch = pcpu->intr_stack_top (offset 8) for trap handler.
         * _trap_entry expects sscratch = interrupt stack pointer, not pcpu.
         */
        "ld    t0, 8(a1)\n"
        "csrw  sscratch, t0\n"

        /* Load sp from pcpu->intr_stack_top (offset 8). */
        "ld    sp, 8(a1)\n"

        /* Load shared satp and activate paging. */
        ".option push\n"
        ".option norelax\n"
        "la    t0, _shared_satp\n"
        ".option pop\n"
        "ld    t0, 0(t0)\n"
        "sfence.vma zero, zero\n"
        "csrw  satp, t0\n"
        "sfence.vma zero, zero\n"

        /* Call C init with pcpu pointer as argument. */
        "mv    a0, a1\n"
        "call  secondary_init_c\n"

        /* Should not return; safety net. */
        "1:\n"
        "wfi\n"
        "j     1b\n");
}

void smp_boot_secondaries(void)
{
    if (!sbi_has_hsm) {
        printk(KERN_WARNING, STR("SBI HSM not available, cannot start "
                                 "secondaries\n"));
        return;
    }

    /* Save BSP's satp for secondaries to load. */
    __asm__ volatile("csrr %0, satp" : "=r"(_shared_satp));

    u32 nr_harts = board_info.nr_harts;
    if (nr_harts == 0)
        nr_harts = 1; /* FDT parse may have failed */
    if (nr_harts > MAX_CPUS)
        nr_harts = MAX_CPUS;

    /* _bsp_hartid is saved by _start (entry.c) - OpenSBI may pick any hart as
     * the boot hart, not necessarily hart 0.
     */
    extern u64 _bsp_hartid;
    u32 bsp = (u32) _bsp_hartid;

    printk(KERN_INFO, STR("SMP: %u harts, BSP=hart %u, MAX_CPUS=%u\n"),
           (unsigned) nr_harts, (unsigned) bsp, (unsigned) MAX_CPUS);

    /* __boot_ap is the entry.c fallback array - secondaries that lost the hart
     * lottery spin on __boot_ap[hartid] until the BSP writes their pcpu pointer
     * there. Used when SBI HSM returns ALREADY_STARTED.
     */
    extern volatile u64 __boot_ap[];

    u32 started = 0;
    u32 cpuid = 1; /* logical CPU index for secondaries */
    for (u32 i = 0; i < nr_harts; i++) {
        if (i == bsp)
            continue; /* skip the BSP */

        if (cpuid >= MAX_CPUS || i >= 128)
            break;

        /* Initialize pcpu for this hart. */
        pcpu_array[cpuid].hartid = i;
        pcpu_array[cpuid].cpuid = cpuid;
        pcpu_array[cpuid].intr_stack_top =
            (u64 *) &intr_stacks[cpuid][INTR_STACK_SIZE];
        pcpu_array[cpuid].online = false;
        pcpu_array[cpuid].idle = false;
        pcpu_array[cpuid].critnest = 0;
        pcpu_array[cpuid].pending_ipis = 0;

        struct sbiret ret = sbi_hart_start(i, (u64) (uptr) _secondary_start,
                                           (u64) (uptr) &pcpu_array[cpuid]);

        if (ret.error == SBI_SUCCESS) {
            started++;
        } else if (ret.error == SBI_ERR_ALREADY_AVAILABLE) {
            /* Hart is running (spinning in the entry lottery loop).
             * Release it via __boot_ap.
             */
            __atomic_store_n(&__boot_ap[i], (u64) (uptr) &pcpu_array[cpuid],
                             __ATOMIC_RELEASE);
            started++;
        } else {
            printk(KERN_WARNING, STR("SBI hart_start(%u) failed: %ld\n"),
                   (unsigned) i, (long) ret.error);
        }
        cpuid++;
    }

    if (started == 0) {
        printk(KERN_INFO, STR("SMP: no secondary harts started\n"));
        return;
    }

    /* Busy-wait for secondaries to come online (timeout ~1s). */
    u32 expected = 1 + started;
    for (u32 t = 0; t < 10000; t++) {
        if (nr_cpus_online >= expected)
            break;
        for (volatile u32 d = 0; d < 10000; d++)
            ; /* short spin delay */
    }

    printk(KERN_INFO, STR("SMP: %u/%u harts online\n"),
           (unsigned) nr_cpus_online, (unsigned) nr_harts);
}

/* IPI implementation */

void ipi_send(u32 cpuid, u32 type)
{
    if (cpuid >= MAX_CPUS)
        return;
    struct pcpu *target = &pcpu_array[cpuid];

    /* Atomically OR the type into the target's pending_ipis.
     * Use .aqrl so the bit is globally visible before sbi_send_ipi() triggers
     * the software interrupt on the target hart.
     */
    __asm__ volatile("amoor.w.aqrl zero, %1, (%0)"
                     :
                     : "r"(&target->pending_ipis), "r"(type)
                     : "memory");

    /* Send the actual software interrupt via SBI. */
    sbi_send_ipi(1, target->hartid);
}

/* Send an IPI to all CPUs in mask.  Converts logical CPU mask to a
 * physical hart mask and issues a single batched SBI ecall.
 *
 * SBI IPI uses hart_mask relative to hart_mask_base.  For simplicity
 * we use base=0 and guard against hartid >= 64 (which would be UB
 * in the shift).  Harts with id >= 64 fall back to individual ecalls.
 */
void ipi_send_mask(cpumask_t mask, u32 type)
{
    if (mask == CPUMASK_NONE)
        return;

    /* OR the type into each target's pending_ipis. */
    u32 cpu;
    u64 hart_mask = 0;
    cpumask_for_each(mask, cpu)
    {
        struct pcpu *target = &pcpu_array[cpu];
        __asm__ volatile("amoor.w.aqrl zero, %1, (%0)"
                         :
                         : "r"(&target->pending_ipis), "r"(type)
                         : "memory");
        if (target->hartid < 64)
            hart_mask |= (u64) 1 << target->hartid;
        else
            sbi_send_ipi(1, target->hartid);
    }

    /* Single batched SBI ecall for all harts with id < 64. */
    if (hart_mask)
        sbi_send_ipi(hart_mask, 0);
}

void ipi_send_broadcast(u32 type)
{
    cpumask_t targets =
        cpumask_andnot(mp_get_active_cpus(), (cpumask_t) 1 << get_cpuid());
    ipi_send_mask(targets, type);
}

void ipi_handle(void)
{
    struct pcpu *pc = get_pcpu();

    /* Atomically swap pending_ipis with 0. */
    u32 pending;
    __asm__ volatile("amoswap.w.aq %0, zero, (%1)"
                     : "=r"(pending)
                     : "r"(&pc->pending_ipis)
                     : "memory");

    if (pending & IPI_TLB)
        __asm__ volatile("sfence.vma zero, zero" ::: "memory");

    if (pending & IPI_SCHED) {
        /* Signal need_resched so trap_dispatch handles the preemption.
         * This ensures need_resched is cleared properly and prevents leaks.
         */
        __atomic_store_n(&pc->need_resched, 1, __ATOMIC_RELEASE);
    }

    if (pending & IPI_TIMER) {
        /* Remote cross-hart callout cancellation removed the list head.
         * Recompute the local merged deadline and reprogram the HW timer
         * so no stale deadline fires a spurious interrupt.
         */
        callout_reprogram_local();
    }
}
