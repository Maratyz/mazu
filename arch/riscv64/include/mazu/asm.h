/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ASM_H
#define MAZU_ASM_H

#include <mazu/barrier.h>
#include <mazu/base.h>

/* RISC-V S-mode CSR bit definitions (sstatus). */
#define SSTATUS_SIE BIT(1)  /* supervisor interrupt enable */
#define SSTATUS_SPIE BIT(5) /* previous interrupt enable */
#define SSTATUS_SPP BIT(8)  /* supervisor previous privilege */
#define SSTATUS_SUM BIT(18) /* supervisor user memory access */
#define SSTATUS_MXR BIT(19) /* make executable readable */

/* sie register bits. */
#define SIE_SSIE BIT(1) /* supervisor software interrupt enable (IPI) */
#define SIE_STIE BIT(5) /* supervisor timer interrupt enable */
#define SIE_SEIE BIT(9) /* supervisor external interrupt enable (PLIC) */

/* sip register bits. */
#define SIP_SSIP BIT(1) /* supervisor software interrupt pending */

#define halt_execution()             \
    do {                             \
        while (true)                 \
            __asm__ volatile("wfi"); \
        __builtin_unreachable();     \
    } while (0)

static inline void hlt(void)
{
    __asm__ volatile("wfi");
}

/* Pseudo-random number generator seeded from the hardware time CSR.
 * Not cryptographically secure; suitable only for non-security uses
 * such as ISN generation jitter and hash table salting.
 */
static inline bool pseudo_rand_u64(u64 *result)
{
    if (!result)
        return false;
    u64 t;
    __asm__ volatile("csrr %0, time" : "=r"(t));
    /* Xorshift mix: extract entropy from the hardware time counter. */
    t ^= t << 21;
    t ^= t >> 35;
    t ^= t << 4;
    *result = t;
    return true;
}

static inline void disable_interrupts(void)
{
    __asm__ volatile("csrci sstatus, 2" : : : "memory");
}

static inline void enable_interrupts(void)
{
    __asm__ volatile("csrsi sstatus, 2" : : : "memory");
}

static inline u32 mmio_read32(u64 addr)
{
    u32 val;
    __asm__ volatile("fence io,io; lw %0, 0(%1)"
                     : "=r"(val)
                     : "r"(addr)
                     : "memory");
    return val;
}
static inline void mmio_write32(u64 addr, u32 val)
{
    __asm__ volatile("sw %1, 0(%0); fence io,io"
                     :
                     : "r"(addr), "r"(val)
                     : "memory");
}

static inline u8 mmio_read8(u64 addr)
{
    u8 val;
    __asm__ volatile("fence io,io; lb %0, 0(%1)"
                     : "=r"(val)
                     : "r"(addr)
                     : "memory");
    return val;
}
static inline void mmio_write8(u64 addr, u8 val)
{
    __asm__ volatile("sb %1, 0(%0); fence io,io"
                     :
                     : "r"(addr), "r"(val)
                     : "memory");
}

/* Trigger a supervisor software interrupt on the current hart.
 * Used as the local reschedule/self-kick mechanism because S-mode ecall does
 * not return through the kernel trap path.
 */
static inline void trigger_ssi(void)
{
    __asm__ volatile("csrs sip, %0" : : "r"(SIP_SSIP) : "memory");
}

/* SUM bit toggling for S-mode user-memory access. */
static inline void uaccess_enable(void)
{
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
}

static inline void uaccess_disable(void)
{
    __asm__ volatile("csrc sstatus, %0" : : "r"(SSTATUS_SUM) : "memory");
}

#endif /* MAZU_ASM_H */
