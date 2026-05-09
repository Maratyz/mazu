/* SPDX-License-Identifier: MIT */
#include <isr.h>
#include <mazu/asm.h>
#include <mazu/selftest.h>

extern void _trap_entry(void);

static i32 test_stvec_correct(void)
{
    u64 stvec;
    __asm__ volatile("csrr %0, stvec" : "=r"(stvec));
    /* Direct mode: low 2 bits = 0, address = _trap_entry. */
    return (stvec == (u64) (uptr) _trap_entry) ? 0 : 1;
}
DEFINE_SELFTEST(stvec_correct, test_stvec_correct);

static i32 test_satp_sv39(void)
{
    u64 satp;
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    /* Mode field (bits 63:60) must be 8 for Sv39. */
    return ((satp >> 60) == 8) ? 0 : 1;
}
DEFINE_SELFTEST(satp_sv39, test_satp_sv39);

static i32 test_sstatus_hardened(void)
{
    u64 sstatus;
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    /* SUM and MXR must both be 0. */
    return ((sstatus & (SSTATUS_SUM | SSTATUS_MXR)) == 0) ? 0 : 1;
}
DEFINE_SELFTEST(sstatus_hardened, test_sstatus_hardened);
