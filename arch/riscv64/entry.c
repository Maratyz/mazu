/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit kernel entry point.
 *
 * OpenSBI (M-mode) hands control here in supervisor mode.
 *   a0 = hart ID
 *   a1 = physical address of the device tree blob (FDT)
 *
 * Responsibilities:
 *   1. Disable supervisor interrupts until arch_init() sets stvec.
 *   2. Run an atomic lottery so only one hart (BSP) proceeds.
 *   3. Set gp to point at pcpu_array[0] (BSP per-CPU data).
 *   4. Point sp at the top of the boot stack.
 *   5. Save the FDT pointer before BSS zeroing.
 *   6. Zero the .bss section.
 *   7. Initialize BSP's sscratch to its pcpu address.
 *   8. Call kernel_init(), the portable C entry point.
 *
 * Secondary harts (under OpenSBI HSM) never reach _start; they are
 * started later via sbi_hart_start() into _secondary_start.  The lottery
 * exists for robustness against firmware that starts multiple harts.
 */

#include <mazu/base.h>

extern void kernel_init(void) __attribute__((noreturn));

#define BOOT_DATA(align) __section(".data") __aligned(align) __used

/* Boot stack: 16 KiB, 16-byte aligned, in .data.
 * Stacks grow downward; sp is initialized to &_boot_stack[sizeof(_boot_stack)].
 */
BOOT_DATA(16)
static char _boot_stack[0x4000] __asm__("_boot_stack");

/* Hart lottery: the first hart to atomically increment this from 0 wins
 * and becomes the BSP.  Must be in .data (survives BSS zeroing).
 */
BOOT_DATA(4)
static volatile u32 _boot_lottery __asm__("_boot_lottery") = 0;

/* BSP hart ID saved by _start. OpenSBI may choose any hart as the boot hart
 * (e.g. hart 1 instead of hart 0). smp_boot_secondaries() needs this to avoid
 * trying to re-start the BSP and to correctly enumerate all secondary harts.
 * Must be in .data (written before BSS zeroing).
 */
#if CONFIG_SMP
BOOT_DATA(8)
u64 _bsp_hartid __asm__("_bsp_hartid") = 0;

/* Per-hart boot-release slots.  BSP writes the pcpu pointer here to wake
 * a secondary that lost the lottery (non-HSM fallback path, or when HSM
 * returns SBI_ERR_ALREADY_AVAILABLE because firmware started all harts).
 * Must be in .data to survive BSS zeroing.
 */
BOOT_DATA(8)
volatile u64 __boot_ap[128] __asm__("__boot_ap") = {0};
#endif

/* Placed first in .text.entry so it lands exactly at the kernel load
 * address (0x80200000) that OpenSBI jumps to.
 *
 * naked: compiler must not emit any prologue or epilogue; sp is invalid until
 * it is pointed at _boot_stack.
 */
__attribute__((naked, section(".text.entry"), noreturn)) void _start(void)
{
    __asm__(
        /* Disable supervisor-mode interrupts (clear SIE, sstatus bit 1). */
        "csrci sstatus, 2\n"

        /* Belt-and-suspenders: ensure the MMU is off (satp = 0). */
        "csrw  satp, zero\n"

        /* Atomic hart lottery: only one hart proceeds as BSP.
         * Under OpenSBI HSM only one hart reaches _start, so this always
         * succeeds. The lottery handles non-HSM firmware that may start
         * multiple harts simultaneously.
         */
        ".option push\n"
        ".option norelax\n"
        "la    t0, _boot_lottery\n"
        ".option pop\n"
        "li    t1, 1\n"
        "amoadd.w.aq t2, t1, (t0)\n" /* t2 = old value; 0 = this hart is BSP */
        "bnez  t2, _secondary_spin\n"

#if CONFIG_SMP
        /* BSP path: save the real hartid (a0) before anything clobbers it.
         * _bsp_hartid is in .data so it survives BSS zeroing.
         */
        "la    t0, _bsp_hartid\n"
        "sd    a0, 0(t0)\n"
#endif

        /* Set gp to pcpu_array[0] (BSP always gets logical CPU 0).
         * With -mno-relax, gp is free for per-CPU use.
         */
        ".option push\n"
        ".option norelax\n"
        "la    gp, pcpu_array\n"
        ".option pop\n"

        /* Set sscratch = boot stack top as the early interrupt stack.
         * _trap_entry expects sscratch = interrupt stack pointer, not pcpu.
         * interrupt_init() overwrites this with intr_stack_top later.
         */
        "la    t0, _boot_stack\n"
        "li    t1, 0x4000\n"
        "add   t0, t0, t1\n"
        "csrw  sscratch, t0\n"

        /* Set sp to the top of the boot stack.
         * 0x4000 does not fit in a 12-bit addi immediate, so use li + add.
         */
        "la    t0, _boot_stack\n"
        "li    t1, 0x4000\n"
        "add   sp, t0, t1\n"

        /* Save a1 (FDT pointer from OpenSBI) into .data before BSS zeroing,
         * which would clobber a BSS variable.
         */
        "la    t2, _fdt_addr\n"
        "sd    a1, 0(t2)\n"

        /* Zero .bss word-by-word (QEMU does not guarantee it). */
        "la    t0, _bss_start\n"
        "la    t1, _bss_end\n"
        "1:\n"
        "bgeu  t0, t1, 2f\n"
        "sd    zero, 0(t0)\n"
        "addi  t0, t0, 8\n"
        "j     1b\n"
        "2:\n"

        /* Transfer to the portable C entry point (noreturn). */
        "call  kernel_init\n"

        /* kernel_init() never returns; spin with wfi as a safety net. */
        "3:\n"
        "wfi\n"
        "j     3b\n"

        /* Secondary hart spin-wait.
         * Hart ID is in a0.  Spin until __boot_ap[a0] becomes nonzero. On SMP
         * builds, BSP stores the pcpu pointer there; the secondary loads it
         * into a1 and jumps to _secondary_start (same ABI as SBI HSM entry). On
         * UP builds, just park in wfi (no secondary boot path).
         */
        "_secondary_spin:\n"
#if CONFIG_SMP
        /* Bounds check: __boot_ap has 128 slots.  Park if hartid >= 128. */
        "li    t0, 128\n"
        "bgeu  a0, t0, 5f\n"
        ".option push\n"
        ".option norelax\n"
        "la    t0, __boot_ap\n"
        ".option pop\n"
        "slli  t1, a0, 3\n" /* t1 = hartid * 8 (u64 slots) */
        "add   t0, t0, t1\n"
        "4:\n"
        "ld    t1, 0(t0)\n"
        "beqz  t1, 4b\n"
        "fence r, rw\n"
        /* a0 = hartid (already set), a1 = pcpu pointer from __boot_ap */
        "mv    a1, t1\n"
        "j     _secondary_start\n"
        /* Fallthrough park for out-of-range hart IDs. */
        "5:\n"
        "wfi\n"
        "j     5b\n"
#else
        "4:\n"
        "wfi\n"
        "j     4b\n"
#endif
    );
}
