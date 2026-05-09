/* SPDX-License-Identifier: MIT */
/* RISC-V trap entry and dispatch.
 *
 * Hard-RT invariants enforced here:
 * - every trap runs on the per-hart interrupt stack with interrupts masked
 * - each entry resolves exactly one cause, then converges on schedule_decision
 * - trap exit restores exactly one trap frame: either the current task's or the
 *   next task selected by sched_schedule()
 * - need_resched is consumed only at the schedule_decision gate
 */

#include <fdt.h>
#include <isr.h>
#include <mazu/asm.h>
#include <mazu/base.h>
#include <mazu/callout.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/irqchip.h>
#include <mazu/irqdesc.h>
#include <mazu/klog.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/syscall.h>
#include <mazu/uaccess.h>

#include "../../kernel/proc/signal.h"

/* Forward declaration for use in backtrace/diagnostic dump. */
static struct str scause_str(u64 cause);

/* Stack backtrace by walking the s0 (frame pointer) chain.
 * On RISC-V with -ggdb, GCC uses s0 as a frame pointer:
 *   [s0-8] = return address (ra)
 *   [s0-16] = caller's s0 (previous frame pointer)
 * Each fp is validated against a plausible kernel address range.
 */

#define KERNEL_TEXT_START 0x80200000UL
#define KERNEL_ADDR_END 0xFFFFFFFFFFFFFFFFUL

static bool is_kernel_addr(u64 addr)
{
    /* Ensure addr is within kernel DRAM and leaves room for fp-relative reads
     * (fp-16). The +16 margin prevents underflow into unmapped regions when the
     * caller reads [fp-8] and [fp-16].
     */
    return addr >= KERNEL_TEXT_START + 16 && addr < KERNEL_ADDR_END;
}

void backtrace_from_fp(u64 fp, u64 pc, int max_depth)
{
    printk(KERN_ERR, STR("backtrace:\n"));
    printk(KERN_ERR, STR("  #0 pc=%lx\n"), pc);

    for (int i = 1; i < max_depth; i++) {
        if (fp == 0 || (fp & 7) != 0 || !is_kernel_addr(fp))
            break;

        /* Read return address and previous fp from stack frame. */
        u64 ra = *(u64 *) (uptr) (fp - 8);
        u64 prev_fp = *(u64 *) (uptr) (fp - 16);

        if (ra == 0)
            break;

        printk(KERN_ERR, STR("  #%ld ra=%lx\n"), (i64) i, ra);

        /* Chain terminates when previous fp is null or doesn't ascend. */
        if (prev_fp == 0 || prev_fp <= fp)
            break;

        fp = prev_fp;
    }
}

void trap_dump_diagnostic(const struct trap_frame *tf,
                          u32 cpu,
                          u16 pid,
                          u16 tid)
{
    printk(KERN_ERR,
           STR("--- trap diagnostic dump ---\n"
               "cpu=%lu tid=%hu pid=%hu scause=%lx (%s)\n"
               "sepc=%lx stval=%lx sstatus=%lx\n"),
           (u64) cpu, (u32) tid, (u32) pid, tf->scause, scause_str(tf->scause),
           tf->sepc, tf->stval, tf->sstatus);
    printk(KERN_ERR,
           STR("ra=%lx sp=%lx gp=%lx tp=%lx\n"
               "a0=%lx a1=%lx a2=%lx a3=%lx a4=%lx a5=%lx a6=%lx a7=%lx\n"
               "s0=%lx s1=%lx s2=%lx s3=%lx\n"),
           tf->ra, tf->sp, tf->gp, tf->tp, tf->a0, tf->a1, tf->a2, tf->a3,
           tf->a4, tf->a5, tf->a6, tf->a7, tf->s0, tf->s1, tf->s2, tf->s3);

    /* Only walk the frame-pointer chain for S-mode faults. For U-mode faults
     * tf->s0 is user-controlled and could point anywhere.
     */
    if (tf->sstatus & SSTATUS_SPP)
        backtrace_from_fp(tf->s0, tf->sepc, 16);

    printk(KERN_ERR, STR("--- end diagnostic dump ---\n"));
}

/* PLIC register layout */

#define PLIC_DEFAULT_BASE 0x0c000000UL

/* PLIC context: each hart has 2 contexts (M-mode=2*hartid, S-mode=2*hartid+1).
 * The kernel runs in S-mode, so context = 2*hartid + 1.
 *
 * This matches QEMU virt layout used by kernel. Hardware with different PLIC
 * context layout would need FDT-driven discovery instead of fixed mapping.
 */
static inline int plic_context(void)
{
#if CONFIG_SMP
    return 2 * (int) get_hartid() + 1;
#else
    return 1; /* hart 0, S-mode */
#endif
}

/* Runtime PLIC base, set from FDT or fallback in interrupt_init(). */
static u64 plic_base;

#if CONFIG_SMP
#include <mazu/spinlock.h>
static spinlock_t plic_enable_lock = SPINLOCK_INITIALIZER;
#endif

static inline u64 plic_priority(int src)
{
    return plic_base + (u64) (4 * src);
}
static inline u64 plic_enable(int ctx, int w)
{
    return plic_base + 0x2000 + (u64) (0x80 * ctx) + (u64) (4 * w);
}
static inline u64 plic_threshold(int ctx)
{
    return plic_base + 0x200000 + (u64) (0x1000 * ctx);
}
static inline u64 plic_claim_reg(int ctx)
{
    return plic_base + 0x200004 + (u64) (0x1000 * ctx);
}

/* Verify that the C definition of struct trap_frame matches asm frame layout
 * assumed by '_trap_entry'. If either changes without the other being updated,
 * these assertions will catch the mismatch at compile time.
 */
static_assert(sizeof(struct trap_frame) == 35UL * 8,
              /* Error: */
              "trap_frame size must match _trap_entry asm frame "
              "(35 * 8 bytes)");
static_assert(offsetof(struct trap_frame, sepc) == 31UL * 8,
              /* Error: */
              "trap_frame.sepc must be at slot 31");
static_assert(offsetof(struct trap_frame, scause) == 32UL * 8,
              /* Error: */
              "trap_frame. scause must be at slot 32");
static_assert(offsetof(struct trap_frame, stval) == 33UL * 8,
              /* Error: */
              "trap_frame.stval must be at slot 33");
static_assert(offsetof(struct trap_frame, sstatus) == 34UL * 8,
              /* Error: */
              "trap_frame. sstatus must be at slot 34");

/* scause values */

#define SCAUSE_INT_BIT (1ULL << 63)
#define SCAUSE_EXTI (SCAUSE_INT_BIT | 9)   /* supervisor external interrupt */
#define SCAUSE_STIMER (SCAUSE_INT_BIT | 5) /* supervisor timer interrupt */
#define SCAUSE_SSI (SCAUSE_INT_BIT | 1)    /* supervisor software interrupt */

/* Synchronous exception codes. */
#define SCAUSE_ECALL_U 8 /* ecall from U-mode */
#define SCAUSE_ECALL_S 9 /* ecall from S-mode */
#define SCAUSE_INST_PAGE_FAULT 12
#define SCAUSE_LOAD_PAGE_FAULT 13
#define SCAUSE_STORE_PAGE_FAULT 15

/* Human-readable names for scause values (RISC-V privileged spec Table 4.2).
 * Used in diagnostic output so developers do not need to decode raw numbers.
 */
static struct str scause_str(u64 cause)
{
    u64 code = cause & ~SCAUSE_INT_BIT;
    if (cause & SCAUSE_INT_BIT) {
        switch (code) {
        case 1:
            return STR("supervisor software interrupt");
        case 5:
            return STR("supervisor timer interrupt");
        case 9:
            return STR("supervisor external interrupt");
        default:
            return STR("unknown interrupt");
        }
    }
    switch (code) {
    case 0:
        return STR("instruction address misaligned");
    case 1:
        return STR("instruction access fault");
    case 2:
        return STR("illegal instruction");
    case 3:
        return STR("breakpoint");
    case 4:
        return STR("load address misaligned");
    case 5:
        return STR("load access fault");
    case 6:
        return STR("store address misaligned");
    case 7:
        return STR("store access fault");
    case 8:
        return STR("ecall from U-mode");
    case 9:
        return STR("ecall from S-mode");
    case 12:
        return STR("instruction page fault");
    case 13:
        return STR("load page fault");
    case 15:
        return STR("store page fault");
    default:
        return STR("unknown exception");
    }
}

/* Installed in stvec (direct mode, low 2 bits = 0).
 *
 * Uses sscratch to swap to the per-hart interrupt stack on entry. Saves all 31
 * GPRs + sepc + scause + stval + sstatus (35 x 8 = 280 bytes) into a struct
 * trap_frame on the interrupt stack.
 *
 * Calls trap_dispatch(tf) which returns trap_frame pointer - possibly pointing
 * to a different task's frame if a context switch occurred.
 *
 * Restores from the returned frame, reloads sscratch from pcpu->intr_stack_top
 * (gp+8), and srets into the (potentially new) task.
 */
__attribute__((naked, aligned(4))) void _trap_entry(void)
{
    __asm__(
        /* Swap sp with sscratch: sp = intr_stack, sscratch = task_sp. */
        "csrrw sp, sscratch, sp\n"

        /* Allocate the trap frame on the interrupt stack. */
        "addi sp, sp, -(35*8)\n"

        /* Save t0 first to free it for use as scratch. */
        "sd   x5,  4*8(sp)\n"

        /* Read old task sp from sscratch and save it. */
        "csrr x5, sscratch\n"
        "sd   x5,  1*8(sp)\n" /* sp (x2) = task sp */

        /* Save all remaining GPRs. */
        "sd   x1,  0*8(sp)\n" /* ra  */
        "sd   x3,  2*8(sp)\n" /* gp  */
        "sd   x4,  3*8(sp)\n" /* tp  */
        /* x5 (t0) already saved above */
        "sd   x6,  5*8(sp)\n" /* t1  */
        "sd   x7,  6*8(sp)\n" /* t2  */
        "sd   x8,  7*8(sp)\n" /* s0  */
        "sd   x9,  8*8(sp)\n" /* s1  */
        "sd  x10,  9*8(sp)\n" /* a0  */
        "sd  x11, 10*8(sp)\n" /* a1  */
        "sd  x12, 11*8(sp)\n" /* a2  */
        "sd  x13, 12*8(sp)\n" /* a3  */
        "sd  x14, 13*8(sp)\n" /* a4  */
        "sd  x15, 14*8(sp)\n" /* a5  */
        "sd  x16, 15*8(sp)\n" /* a6  */
        "sd  x17, 16*8(sp)\n" /* a7  */
        "sd  x18, 17*8(sp)\n" /* s2  */
        "sd  x19, 18*8(sp)\n" /* s3  */
        "sd  x20, 19*8(sp)\n" /* s4  */
        "sd  x21, 20*8(sp)\n" /* s5  */
        "sd  x22, 21*8(sp)\n" /* s6  */
        "sd  x23, 22*8(sp)\n" /* s7  */
        "sd  x24, 23*8(sp)\n" /* s8  */
        "sd  x25, 24*8(sp)\n" /* s9  */
        "sd  x26, 25*8(sp)\n" /* s10 */
        "sd  x27, 26*8(sp)\n" /* s11 */
        "sd  x28, 27*8(sp)\n" /* t3  */
        "sd  x29, 28*8(sp)\n" /* t4  */
        "sd  x30, 29*8(sp)\n" /* t5  */
        "sd  x31, 30*8(sp)\n" /* t6  */

        /* Save supervisor CSRs. */
        "csrr x5, sepc\n"
        "sd   x5, 31*8(sp)\n"
        "csrr x5, scause\n"
        "sd   x5, 32*8(sp)\n"
        "csrr x5, stval\n"
        "sd   x5, 33*8(sp)\n"
        "csrr x5, sstatus\n"
        "sd   x5, 34*8(sp)\n"

        /* Call C handler: trap_dispatch(tf) -> returns trap_frame* in a0. */
        "mv   a0, sp\n"
        "call trap_dispatch\n"

        /* a0 points to the frame to restore (possibly a different task). */
        "mv   sp, a0\n"

        /* Restore sstatus and sepc from the frame. */
        "ld   x5, 34*8(sp)\n"
        "csrw sstatus, x5\n"
        "ld   x5, 31*8(sp)\n"
        "csrw sepc, x5\n"

        /* Restore sscratch from pcpu->intr_stack_top (gp + 8).
         * gp is about to be overwritten, so read it now.
         */
        "ld   x5,  2*8(sp)\n" /* x5 = saved gp (pcpu pointer) */
        "ld   x5,  8(x5)\n"   /* x5 = pcpu->intr_stack_top */
        "csrw sscratch, x5\n"

        /* Restore all GPRs except sp. */
        "ld   x1,  0*8(sp)\n" /* ra  */
        /* x2 (sp) restored last */
        "ld   x3,  2*8(sp)\n" /* gp  */
        "ld   x4,  3*8(sp)\n" /* tp  */
        "ld   x5,  4*8(sp)\n" /* t0  */
        "ld   x6,  5*8(sp)\n" /* t1  */
        "ld   x7,  6*8(sp)\n" /* t2  */
        "ld   x8,  7*8(sp)\n" /* s0  */
        "ld   x9,  8*8(sp)\n" /* s1  */
        "ld  x10,  9*8(sp)\n" /* a0  */
        "ld  x11, 10*8(sp)\n" /* a1  */
        "ld  x12, 11*8(sp)\n" /* a2  */
        "ld  x13, 12*8(sp)\n" /* a3  */
        "ld  x14, 13*8(sp)\n" /* a4  */
        "ld  x15, 14*8(sp)\n" /* a5  */
        "ld  x16, 15*8(sp)\n" /* a6  */
        "ld  x17, 16*8(sp)\n" /* a7  */
        "ld  x18, 17*8(sp)\n" /* s2  */
        "ld  x19, 18*8(sp)\n" /* s3  */
        "ld  x20, 19*8(sp)\n" /* s4  */
        "ld  x21, 20*8(sp)\n" /* s5  */
        "ld  x22, 21*8(sp)\n" /* s6  */
        "ld  x23, 22*8(sp)\n" /* s7  */
        "ld  x24, 23*8(sp)\n" /* s8  */
        "ld  x25, 24*8(sp)\n" /* s9  */
        "ld  x26, 25*8(sp)\n" /* s10 */
        "ld  x27, 26*8(sp)\n" /* s11 */
        "ld  x28, 27*8(sp)\n" /* t3  */
        "ld  x29, 28*8(sp)\n" /* t4  */
        "ld  x30, 29*8(sp)\n" /* t5  */
        "ld  x31, 30*8(sp)\n" /* t6  */

        /* Restore task sp from the frame and sret. */
        "ld   sp,  1*8(sp)\n"
        "sret\n");
}

/* PLIC source management, used via the irqchip vtable. */

/* Shared RMW helper for the PLIC enable bitmap. When 'enable' is true the bit
 * is set; otherwise it is cleared. On SMP, updates all S-mode contexts under
 * plic_enable_lock so concurrent calls on different IRQs in the same 32-bit
 * word do not lose updates.
 */
static void plic_set_enable(int irq, bool enable)
{
    if (irq <= 0)
        return;
    mmio_write32(plic_priority(irq), enable ? 1 : 0);
    u32 word = (u32) irq / 32;
    u32 mask = (u32) BIT((u32) irq % 32);
#if CONFIG_SMP
    u32 nr = board_info.nr_harts;
    if (nr == 0)
        nr = 1;
    if (nr > MAX_CPUS)
        nr = MAX_CPUS;
    u64 lflags = spin_lock_irqsave(&plic_enable_lock);
    for (u32 h = 0; h < nr; h++) {
        int ctx = 2 * (int) h + 1;
        u32 old = mmio_read32(plic_enable(ctx, (int) word));
        u32 val = enable ? (old | mask) : (old & ~mask);
        mmio_write32(plic_enable(ctx, (int) word), val);
    }
    spin_unlock_irqrestore(&plic_enable_lock, lflags);
#else
    int ctx = plic_context();
    u32 old = mmio_read32(plic_enable(ctx, (int) word));
    u32 val = enable ? (old | mask) : (old & ~mask);
    mmio_write32(plic_enable(ctx, (int) word), val);
#endif
}

static void plic_irq_enable(int irq)
{
    plic_set_enable(irq, true);
}

static void plic_irq_disable(int irq)
{
    plic_set_enable(irq, false);
}

static u32 plic_claim(void)
{
    return mmio_read32(plic_claim_reg(plic_context()));
}

static void plic_complete(u32 irq)
{
    mmio_write32(plic_claim_reg(plic_context()), irq);
}

static const struct irqchip plic_chip = {
    .enable = plic_irq_enable,
    .disable = plic_irq_disable,
    .claim = plic_claim,
    .complete = plic_complete,
};

const struct irqchip *irqchip;

/* Called from arch_init() before time_init(). */
void interrupt_init(void)
{
    /* Set up the per-hart interrupt stack before installing the trap vector.
     * _trap_entry uses csrrw to swap sp with sscratch on entry, so sscratch
     * must hold intr_stack_top (not the pcpu pointer).
     */
    struct pcpu *pc = get_pcpu();
    pc->intr_stack_top = (u64 *) &intr_stacks[pc->cpuid][INTR_STACK_SIZE];
    __asm__ volatile("csrw sscratch, %0"
                     :
                     : "r"(pc->intr_stack_top)
                     : "memory");

    /* Point stvec at _trap_entry in direct mode (low 2 bits = 0). */
    u64 stvec_val = (u64) (uptr) _trap_entry;
    __asm__ volatile("csrw stvec, %0" : : "r"(stvec_val) : "memory");

    /* Resolve PLIC base from FDT, fall back to QEMU-virt default. */
    plic_base = board_info.plic_base ? board_info.plic_base : PLIC_DEFAULT_BASE;

    /* PLIC: set context threshold to 0 so all priorities are forwarded. */
    mmio_write32(plic_threshold(plic_context()), 0);

    /* Publish the PLIC vtable for irq_desc and other users. */
    irqchip = &plic_chip;

    /* Enable supervisor external interrupt in sie (not sstatus yet). */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_SEIE) : "memory");

    /* Enable supervisor software interrupt for scheduler self-kick (yield) and,
     * on SMP builds, IPI reception. ecall from S-mode goes to M-mode/OpenSBI,
     * so the kernel uses SSI to re-enter trap_dispatch on demand.
     */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_SSIE) : "memory");
}

#if CONFIG_SMP
/* Initialize the PLIC state for a secondary hart before it enters the
 * scheduler.
 */
void interrupt_init_secondary(void)
{
    __asm__ volatile("csrw stvec, %0"
                     :
                     : "r"((u64) (uptr) _trap_entry)
                     : "memory");
    mmio_write32(plic_threshold(plic_context()), 0);
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_SEIE | SIE_SSIE) : "memory");
}
#endif

/* Defined in arch/riscv64/time.c; processes expired callouts on timer IRQ. */
extern void time_handle_timer_interrupt(void);

/* Called from _trap_entry above.
 *
 * Returns the only trap frame that may be restored on this trap exit: either
 * the current task's frame (no switch) or next->td_tf after sched_schedule().
 *
 * Invariants:
 *   - Called with interrupts disabled (sstatus.SIE == 0) and on the per-hart
 *     interrupt stack (sscratch swap in _trap_entry).
 *   - The current frame is saved into td->td_tf before any path that may
 *     switch away from the task or needs a persistent saved frame.
 *   - scause identifies exactly one cause per trap entry; the if-chain is
 *     mutually exclusive (interrupt vs exception vs page fault).
 *   - schedule_decision is the single reschedule gate: every path either
 *     returns the current frame unchanged or calls sched_schedule().
 *   - need_resched is unconditionally drained (atomic exchange to 0) at
 *     schedule_decision.  It only triggers a preemption when the current
 *     task is still RUNNING; otherwise the flag is silently consumed so it
 *     never leaks to the next scheduled task.
 */
struct trap_frame *trap_dispatch(struct trap_frame *tf)
{
    struct pcpu *pc = get_pcpu();
    struct sched_task *td = pc->curthread;
    struct trap_frame *active_tf = tf;
    bool frame_saved = false;

    pc->trap_nest++;

    u64 scause = tf->scause;
    u16 pid = (td && td->proc) ? td->proc->pid : 0;
    u16 tid = td ? td->id : 0;

    /* Fast syscall path: defer saving the frame until we know a reschedule
     * is required. Other traps snapshot immediately because their handlers
     * consume td->td_tf directly.
     */
    if (td && scause != SCAUSE_ECALL_U) {
        td->td_tf = *tf;
        active_tf = &td->td_tf;
        frame_saved = true;
    }

    /* Interrupts (asynchronous: high bit set in scause) */

    if (scause == SCAUSE_EXTI) {
        pc->irq_nest++;
        pc->nr_exti++;
        u32 source = irqchip->claim();
        if (source != 0) {
            irq_dispatch(source);
            irqchip->complete(source);
        }
        pc->irq_nest--;
    } else if (scause == SCAUSE_STIMER) {
        pc->irq_nest++;
        pc->nr_timer++;
        time_handle_timer_interrupt();
        pc->irq_nest--;
    } else if (scause == SCAUSE_SSI) {
        pc->irq_nest++;
        pc->nr_ssi++;
        /* Clear the supervisor software interrupt pending bit. */
        __asm__ volatile("csrc sip, %0" : : "r"(BIT(1)) : "memory");
#if CONFIG_SMP
        ipi_handle();
#endif
        pc->irq_nest--;

        /* Synchronous exceptions (ecall, page faults): irq_nest stays 0 so
         * syscall handlers may legitimately call kvalloc / block.
         */
    } else if (scause == SCAUSE_ECALL_U) {
        /* ecall from U-mode: dispatch syscall and advance past ecall. */
        if (td) {
            active_tf->sepc += 4;
            active_tf->a0 = (u64) syscall_dispatch(active_tf, td);
        }
    } else if (scause == SCAUSE_ECALL_S) {
        /* ecall from S-mode: advance sepc past the ecall instruction. */
        if (td)
            active_tf->sepc += 4;
    } else if (scause == SCAUSE_INST_PAGE_FAULT ||
               scause == SCAUSE_LOAD_PAGE_FAULT ||
               scause == SCAUSE_STORE_PAGE_FAULT) {
        struct trap_frame *fault_tf = td ? active_tf : tf;

        /* Fault during copy_{to,from}_user: recover to syscall and return
         * -EFAULT from the uaccess entrypoint.
         */
        if (uaccess_handle_page_fault(fault_tf))
            goto schedule_decision;

        if (scause == SCAUSE_INST_PAGE_FAULT && td &&
            !(tf->sstatus & SSTATUS_SPP) && td->proc &&
            signal_handle_trampoline_fault(td->proc, active_tf))
            goto schedule_decision;

        /* If the fault came from user mode, kill the process instead of
         * panicking the whole kernel.
         */
        if (td && !(tf->sstatus & SSTATUS_SPP) && td->proc) {
            klog_fault_event("user_page_fault", pc->cpuid, pid, tid, scause,
                             tf->sepc, tf->stval, tf->sstatus);
            trap_dump_diagnostic(active_tf, pc->cpuid, pid, tid);
            td->proc->exit_code = -1;
            td->proc->task = NULL;
            struct proc *parent = proc_find(td->proc->parent_pid);
            bool is_orphan = !parent || parent->state == PROC_STATE_ZOMBIE;
            proc_set_state(td->proc, PROC_STATE_ZOMBIE);
            if (is_orphan)
                proc_free(td->proc);
            else
                proc_notify_parent(td->proc);
            td->proc = NULL;
            sched_set_task_state(td, TD_STATE_TERMINATING);
        } else if (td && !td->must_not_exit && td->callback) {
            /* Kernel task fault from a non-critical task: kill the task rather
             * than panicking the entire kernel. Infrastructure-critical tasks
             * (idle, watchdog) have must_not_exit set.
             */
            klog_fault_event("kernel_task_fault", pc->cpuid, pid, tid, scause,
                             tf->sepc, tf->stval, tf->sstatus);
            trap_dump_diagnostic(active_tf, pc->cpuid, pid, tid);
            printk(KERN_ERR, STR("killing non-critical kernel task id=%hu\n"),
                   (u32) td->id);
            sched_set_task_state(td, TD_STATE_TERMINATING);
        } else {
            klog_fault_event("kernel_page_fault", pc->cpuid, pid, tid, scause,
                             tf->sepc, tf->stval, tf->sstatus);
            trap_dump_diagnostic(td ? active_tf : tf, pc->cpuid, pid, tid);
            halt_execution();
        }
    } else if (scause == 3) {
        /* ebreak: breakpoint. With CONFIG_UBSAN this is an undefined behavior
         * trap inserted by -fsanitize-trap=all. Without UBSAN, it is still a
         * fatal illegal operation (explicit ebreak or __builtin_trap). Either
         * way, dump diagnostics and halt.
         */
        klog_fault_event("ubsan_trap", pc->cpuid, pid, tid, scause, tf->sepc,
                         tf->stval, tf->sstatus);
        printk(KERN_ERR,
               STR("*** UBSan/ebreak trap at sepc=%lx (cpu=%lu tid=%hu) ***\n"),
               td ? active_tf->sepc : tf->sepc, (u64) pc->cpuid, (u32) tid);
        trap_dump_diagnostic(td ? active_tf : tf, pc->cpuid, pid, tid);
        halt_execution();
    } else {
        klog_fault_event("unexpected_trap", pc->cpuid, pid, tid, scause,
                         tf->sepc, tf->stval, tf->sstatus);
        trap_dump_diagnostic(td ? active_tf : tf, pc->cpuid, pid, tid);
        halt_execution();
    }

    /* Signal delivery: check for pending signals before returning to user mode.
     * Must run after exception handling but before the scheduler decision so
     * that the handler is invoked in the correct task context.
     */
    if (td && td->proc && td->state != TD_STATE_TERMINATING &&
        !(active_tf->sstatus & SSTATUS_SPP) &&
        signal_has_deliverable(td->proc)) {
        signal_deliver(td->proc, active_tf);
    }

/* Scheduler decision */
schedule_decision:
    /* Unconditionally consume need_resched so the flag never leaks to the
     * next task.  If the current task is still RUNNING and the flag was set,
     * demote it to READY so sched_schedule() picks a replacement.
     */
    {
        bool resched =
            __atomic_exchange_n(&pc->need_resched, 0, __ATOMIC_ACQUIRE);
        if (td && td->state == TD_STATE_RUNNING && resched)
            sched_set_task_state(td, TD_STATE_READY);
        if (td && !frame_saved && td->state != TD_STATE_RUNNING) {
            td->td_tf = *active_tf;
            active_tf = &td->td_tf;
        }
    }

    if (!td || td->state == TD_STATE_RUNNING) {
        /* No context switch needed; return same task's frame. */
        pc->trap_nest--;
        return active_tf;
    }

    /* The current task already left RUNNING (sleep/yield/terminate/preempt).
     * sched_schedule() now owns the only path that may pick a replacement.
     *
     * satp elision: all tasks share the kernel's identity-mapped page table.
     * User-mode pages are mapped into the single page table via
     * proc_map_user_page, so no satp write or sfence.vma is needed on context
     * switch. When per-process address spaces are introduced, compare
     * old->proc->satp with next->proc->satp and skip the satp write +
     * sfence.vma when they match.
     */
    DEBUG_ASSERT(td == pc->curthread);
    DEBUG_ASSERT(td->state != TD_STATE_RUNNING);
    struct sched_task *next = sched_schedule(td);
    pc->trap_nest--;
    return &next->td_tf;
}
