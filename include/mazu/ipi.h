/* SPDX-License-Identifier: MIT */
/* Inter-Processor Interrupt (IPI) types and dispatch.
 *
 * On SMP, ipi_send() atomically ORs a type flag into the target hart's
 * pending_ipis field and triggers an SBI software interrupt.
 * ipi_handle() is called from the trap handler on SCAUSE_SSI.
 *
 * On UP, all IPI functions compile to nothing.
 */

#ifndef MAZU_IPI_H
#define MAZU_IPI_H

#include <mazu/base.h>
#include <mazu/cpumask.h>

/* IPI type flags (bitmask). */
#define IPI_SCHED BIT(0) /* reschedule request */
#define IPI_TLB BIT(1)   /* TLB shootdown (sfence.vma) */
#define IPI_TIMER BIT(2) /* recompute merged callout deadline */

#if CONFIG_SMP

/* Send an IPI of the given type(s) to a specific CPU (logical index). */
void ipi_send(u32 cpuid, u32 type);

/* Send an IPI to all CPUs in mask (single batched SBI ecall). */
void ipi_send_mask(cpumask_t mask, u32 type);

/* Broadcast an IPI of the given type(s) to all other online harts. */
void ipi_send_broadcast(u32 type);

/* Handle pending IPIs on the current hart.  Called from trap handler. */
void ipi_handle(void);

#else /* !CONFIG_SMP */

static inline void ipi_send(u32 cpuid __unused, u32 type __unused) {}
static inline void ipi_send_mask(cpumask_t mask __unused, u32 type __unused) {}
static inline void ipi_send_broadcast(u32 type __unused) {}
static inline void ipi_handle(void) {}

#endif /* CONFIG_SMP */

#endif /* MAZU_IPI_H */
