/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit context-switch primitives.
 *
 * The RISC-V LP64 ABI callee-saved registers are: ra (x1) and s0-s11
 * (x8-x9, x18-x27).  All 13 are saved (104 bytes) plus one padding slot to
 * keep the frame a multiple of 16 bytes (14 × 8 = 112 bytes), satisfying
 * the LP64 ABI 16-byte stack-alignment requirement at every call boundary.
 *
 * Frame layout (low address at sp+0, high address at sp+112):
 *   [0]  ra    [1] s0   [2] s1   [3] s2   [4] s3   [5] s4   [6] s5
 *   [7]  s6    [8] s7   [9] s8  [10] s9  [11] s10  [12] s11  [13] pad
 *
 * sched_create_task() (src/sched.c) initializes the frame so that the
 * first context switch into a new task jumps to sched_task_entry with a
 * clean callee-saved register state.
 */

#include <mazu/base.h>

/* Save the current task's callee-saved registers onto its stack and restore the
 * next task's.
 * @old_sp : pointer to where the current sp should be stored
 * @new_sp : stack pointer of the task to switch to
 *
 * naked: the compiler must not touch sp or any callee-saved register before the
 * asm runs; that would corrupt the very state being saved.
 */
__naked void sched_do_context_switch(u64 **old_sp __unused,
                                     u64 *new_sp __unused)
{
    __asm__(
        /* Allocate frame: 14 slots × 8 bytes = 112 bytes. */
        "addi  sp,  sp, -(14*8)\n"

        /* Save callee-saved registers (ra + s0 through s11). */
        "sd    ra,   0*8(sp)\n"
        "sd    s0,   1*8(sp)\n"
        "sd    s1,   2*8(sp)\n"
        "sd    s2,   3*8(sp)\n"
        "sd    s3,   4*8(sp)\n"
        "sd    s4,   5*8(sp)\n"
        "sd    s5,   6*8(sp)\n"
        "sd    s6,   7*8(sp)\n"
        "sd    s7,   8*8(sp)\n"
        "sd    s8,   9*8(sp)\n"
        "sd    s9,  10*8(sp)\n"
        "sd    s10, 11*8(sp)\n"
        "sd    s11, 12*8(sp)\n"
        /* slot [13] left as padding, not saved */

        /* *old_sp = sp  (publish this task's stack pointer) */
        "sd    sp,  0(a0)\n"

        /* sp = new_sp  (switch to the next task's stack) */
        "mv    sp,  a1\n"

        /* Restore callee-saved registers from the new task's frame. */
        "ld    ra,   0*8(sp)\n"
        "ld    s0,   1*8(sp)\n"
        "ld    s1,   2*8(sp)\n"
        "ld    s2,   3*8(sp)\n"
        "ld    s3,   4*8(sp)\n"
        "ld    s4,   5*8(sp)\n"
        "ld    s5,   6*8(sp)\n"
        "ld    s6,   7*8(sp)\n"
        "ld    s7,   8*8(sp)\n"
        "ld    s8,   9*8(sp)\n"
        "ld    s9,  10*8(sp)\n"
        "ld    s10, 11*8(sp)\n"
        "ld    s11, 12*8(sp)\n"

        /* Free the frame and return into the new task. */
        "addi  sp,  sp, 14*8\n"
        "ret\n");
}

/* Switch sp to new_sp and call fn(arg).
 *
 * Used by sched_enter_secondary to move off the interrupt stack onto the idle
 * task's own stack before entering the idle loop. Without this, _trap_entry's
 * csrrw would overwrite the active call stack (both share intr_stack_top).
 */
__naked __noreturn void sched_call_on_stack(u64 new_sp __unused,
                                            void (*fn)(void *) __unused,
                                            void *arg __unused)
{
    __asm__(
        "mv   sp, a0\n" /* sp = new_sp */
        "mv   a0, a2\n" /* a0 = arg (for fn) */
        "jalr a1\n"     /* call fn(arg) */
        "1: wfi\n"
        "j    1b\n");
}

/* Restore the next task's registers without saving the current task (which is
 * being destroyed).
 *
 * Never returns to the caller - the current task's stack frame is abandoned.
 */
__naked __noreturn void sched_do_final_context_switch(u64 *new_sp __unused)
{
    __asm__(
        /* Switch to the new task's stack; the old stack is discarded. */
        "mv    sp,  a0\n"

        /* Restore callee-saved registers. */
        "ld    ra,   0*8(sp)\n"
        "ld    s0,   1*8(sp)\n"
        "ld    s1,   2*8(sp)\n"
        "ld    s2,   3*8(sp)\n"
        "ld    s3,   4*8(sp)\n"
        "ld    s4,   5*8(sp)\n"
        "ld    s5,   6*8(sp)\n"
        "ld    s6,   7*8(sp)\n"
        "ld    s7,   8*8(sp)\n"
        "ld    s8,   9*8(sp)\n"
        "ld    s9,  10*8(sp)\n"
        "ld    s10, 11*8(sp)\n"
        "ld    s11, 12*8(sp)\n"

        "addi  sp,  sp, 14*8\n"
        "ret\n");
}
