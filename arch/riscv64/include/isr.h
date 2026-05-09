/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ISR_H
#define MAZU_ISR_H

#include <mazu/base.h>

/* Trap frame restored by _trap_entry on trap exit.
 * The layout is fixed to the assembly save/restore order: all 31 GPRs plus the
 * supervisor CSRs consumed by trap_dispatch().
 */
struct trap_frame {
    u64 ra;  /* x1  */
    u64 sp;  /* x2  */
    u64 gp;  /* x3  */
    u64 tp;  /* x4  */
    u64 t0;  /* x5  */
    u64 t1;  /* x6  */
    u64 t2;  /* x7  */
    u64 s0;  /* x8 / fp */
    u64 s1;  /* x9  */
    u64 a0;  /* x10 */
    u64 a1;  /* x11 */
    u64 a2;  /* x12 */
    u64 a3;  /* x13 */
    u64 a4;  /* x14 */
    u64 a5;  /* x15 */
    u64 a6;  /* x16 */
    u64 a7;  /* x17 */
    u64 s2;  /* x18 */
    u64 s3;  /* x19 */
    u64 s4;  /* x20 */
    u64 s5;  /* x21 */
    u64 s6;  /* x22 */
    u64 s7;  /* x23 */
    u64 s8;  /* x24 */
    u64 s9;  /* x25 */
    u64 s10; /* x26 */
    u64 s11; /* x27 */
    u64 t3;  /* x28 */
    u64 t4;  /* x29 */
    u64 t5;  /* x30 */
    u64 t6;  /* x31 */
    u64 sepc, scause, stval, sstatus;
} __packed;

/* Initialize the trap/interrupt entry points for the current hart. */
void interrupt_init(void);

/* Print a stack backtrace by walking the s0 (fp) chain.
 * Starts from the given frame pointer; walks at most max_depth frames.
 * Safe: stops on NULL fp, misaligned fp, or address outside kernel range.
 */
void backtrace_from_fp(u64 fp, u64 pc, int max_depth);

/* Print a full diagnostic dump for a trap frame: registers, cause, and
 * stack backtrace.
 */
void trap_dump_diagnostic(const struct trap_frame *tf,
                          u32 cpu,
                          u16 pid,
                          u16 tid);

#endif /* MAZU_ISR_H */
