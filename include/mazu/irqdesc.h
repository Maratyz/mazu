/* SPDX-License-Identifier: MIT */
/* IRQ descriptor framework.
 *
 * Provides a Linux-style request_irq()/free_irq() API with per-IRQ metadata
 * (handler, arg, name, hit count).  Decouples device drivers from the trap
 * handler and PLIC internals.
 */

#ifndef MAZU_IRQDESC_H
#define MAZU_IRQDESC_H

#include <mazu/base.h>
#include <mazu/errordef.h>

/* Maximum number of IRQ descriptors.  QEMU virt uses PLIC sources 1-8;
 * 64 covers any reasonable expansion without wasting BSS.
 */
#define NR_IRQS 64

/* IRQ handler return values. */
#define IRQ_NONE 0    /* interrupt was not from this device */
#define IRQ_HANDLED 1 /* interrupt was handled by this device */

/* Handler signature: receives the IRQ number and an opaque driver argument.
 * Returns IRQ_HANDLED if the device claimed the interrupt, IRQ_NONE if
 * the interrupt was spurious or not from this device.
 */
typedef int (*irq_handler_t)(int irq, void *arg);

/* Threshold of consecutive unhandled interrupts before the IRQ source is
 * disabled.  Prevents interrupt storms from wedged devices.
 */
#define IRQ_UNHANDLED_THRESHOLD 100

struct irq_desc {
    irq_handler_t handler;
    void *arg;
    const char *name;
    u32 count;
    u32 unhandled_count; /* consecutive IRQ_NONE returns */
    bool disabled_nack;  /* true if disabled due to NACK threshold */
};

/* Register a handler for IRQ source 'irq'.  Enables the source via irqchip.
 * Returns result_ok() on success, result_error(EINVAL) on bad args,
 * result_error(EBUSY) if already registered.
 */
__must_check struct result request_irq(int irq,
                                       irq_handler_t handler,
                                       void *arg,
                                       const char *name);

/* Unregister and disable IRQ source 'irq'. */
void free_irq(int irq);

/* Dispatch an IRQ: look up irq_desc[irq], call handler, bump count.
 * Called from the trap handler after irqchip->claim().
 */
void irq_dispatch(u32 irq);

/* Return a read-only pointer to the descriptor for 'irq' (for future
 * /proc/interrupts).  Returns NULL on out-of-range.
 */
const struct irq_desc *irq_get_desc(int irq);

#endif /* MAZU_IRQDESC_H */
