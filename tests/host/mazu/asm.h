/* SPDX-License-Identifier: MIT */
/* Host-side stub for mazu/asm.h -- replaces RISC-V instructions with no-ops.
 * Used by tests/host/ to compile kernel headers on the build host.
 */
#ifndef MAZU_ASM_H
#define MAZU_ASM_H

#include <mazu/barrier.h>
#include <mazu/base.h>

#include <stdio.h>
#include <stdlib.h>

#define halt_execution()          \
    do {                          \
        fprintf(stderr, "halt\n");\
        abort();                  \
    } while (0)

static inline void hlt(void)
{
}

static inline bool pseudo_rand_u64(u64 *result)
{
    if (!result)
        return false;
    *result = ((u64) rand() << 32) | (u64) rand();
    return true;
}

static inline void disable_interrupts(void)
{
}
static inline void enable_interrupts(void)
{
}

#endif /* MAZU_ASM_H */
