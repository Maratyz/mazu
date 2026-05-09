/* SPDX-License-Identifier: MIT */
/* Interrupt controller vtable.
 *
 * Abstracts the PLIC (or any future interrupt controller) behind a function
 * pointer table so that irq_desc and trap dispatch code are
 * controller-agnostic.
 */

#ifndef MAZU_IRQCHIP_H
#define MAZU_IRQCHIP_H

#include <mazu/base.h>

struct irqchip {
    void (*enable)(int irq);
    void (*disable)(int irq);
    u32 (*claim)(void);
    void (*complete)(u32 irq);
};

/* Set by the arch-specific interrupt init (e.g. PLIC setup in trap.c). */
extern const struct irqchip *irqchip;

#endif /* MAZU_IRQCHIP_H */
