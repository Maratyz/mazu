/* SPDX-License-Identifier: MIT */
/* IRQ descriptor table and registration.
 *
 * Static BSS table of NR_IRQS descriptors.  handler==NULL means the slot
 * is free.  request_irq() is called only during init before secondary
 * harts are started, so no lock is needed.  irq_dispatch() runs in trap
 * context (interrupts disabled) and currently only the BSP handles
 * device IRQs (secondaries enable no PLIC sources), so concurrent
 * access cannot occur.  A spinlock is needed if secondaries begin
 * handling device interrupts.
 *
 * NACK back-off: irq_dispatch tracks consecutive IRQ_NONE returns per
 * source.  After IRQ_UNHANDLED_THRESHOLD consecutive misses, the source
 * is disabled via the irqchip vtable to prevent interrupt storms from
 * wedged or misconfigured devices.
 */

#include <mazu/asm.h>
#include <mazu/barrier.h>
#include <mazu/irqchip.h>
#include <mazu/irqdesc.h>
#include <mazu/print.h>

static struct irq_desc irq_desc[NR_IRQS];

struct result request_irq(int irq,
                          irq_handler_t handler,
                          void *arg,
                          const char *name)
{
    if (irq < 0 || irq >= NR_IRQS || !handler)
        return result_error(EINVAL);

    if (irq_desc[irq].handler)
        return result_error(EBUSY);

    irq_desc[irq].handler = handler;
    irq_desc[irq].arg = arg;
    irq_desc[irq].name = name;
    irq_desc[irq].count = 0;
    irq_desc[irq].unhandled_count = 0;
    irq_desc[irq].disabled_nack = false;

    /* Ensure descriptor stores are visible to all CPUs before the MMIO
     * enable below can let an interrupt through.  The mmio_write32() in
     * irqchip->enable() already includes its own device fence; this
     * barrier orders the normal stores above against other harts that
     * may call irq_dispatch() concurrently.  On UP builds this reduces
     * to a compiler barrier since no other hart can race.
     */
    smp_mb();

    irqchip->enable(irq);

    return result_ok();
}

void free_irq(int irq)
{
    if (irq < 0 || irq >= NR_IRQS)
        return;

    irqchip->disable(irq);

    irq_desc[irq].handler = NULL;
    irq_desc[irq].arg = NULL;
    irq_desc[irq].name = NULL;
    irq_desc[irq].count = 0;
    irq_desc[irq].unhandled_count = 0;
    irq_desc[irq].disabled_nack = false;
}

void irq_dispatch(u32 irq)
{
    if (irq >= NR_IRQS)
        return;

    struct irq_desc *desc = &irq_desc[irq];
    if (!desc->handler)
        return;

    desc->count++;
    int ret = desc->handler((int) irq, desc->arg);

    if (ret == IRQ_HANDLED) {
        desc->unhandled_count = 0;
    } else {
        desc->unhandled_count++;
        if (desc->unhandled_count >= IRQ_UNHANDLED_THRESHOLD &&
            !desc->disabled_nack) {
            desc->disabled_nack = true;
            irqchip->disable((int) irq);
            pr_warn(STR("IRQ %u: %u consecutive unhandled - disabled\n"), irq,
                    desc->unhandled_count);
        }
    }
}

const struct irq_desc *irq_get_desc(int irq)
{
    if (irq < 0 || irq >= NR_IRQS)
        return NULL;
    return &irq_desc[irq];
}

/* Self-tests */

#include __INC_TEST(irqdesc)
