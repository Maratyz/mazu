/* SPDX-License-Identifier: MIT */
#include <mazu/irqdesc.h>
#include <mazu/selftest.h>

/* Test IRQ NACK back-off: a handler returning IRQ_NONE repeatedly
 * should increment unhandled_count and eventually disable the IRQ.
 */

/* Use a high IRQ number unlikely to conflict with real devices. */
#define TEST_IRQ 60

static volatile int selftest_irq_call_count;

static int selftest_irq_handler_none(int irq __unused, void *arg __unused)
{
    selftest_irq_call_count++;
    return IRQ_NONE;
}

static int selftest_irq_handler_ok(int irq __unused, void *arg __unused)
{
    selftest_irq_call_count++;
    return IRQ_HANDLED;
}

/* Verify that IRQ_HANDLED resets the unhandled counter. */
static i32 test_irq_nack_reset(void)
{
    struct result r =
        request_irq(TEST_IRQ, selftest_irq_handler_ok, NULL, "test-ok");
    if (r.is_error)
        return 1;

    selftest_irq_call_count = 0;

    /* Dispatch a few times, all handled. */
    for (int i = 0; i < 5; i++)
        irq_dispatch(TEST_IRQ);

    const struct irq_desc *desc = irq_get_desc(TEST_IRQ);
    if (!desc)
        return 1;

    if (desc->unhandled_count != 0)
        return 1;
    if (selftest_irq_call_count != 5)
        return 1;

    free_irq(TEST_IRQ);
    return 0;
}
DEFINE_SELFTEST(irq_nack_reset, test_irq_nack_reset);

/* Verify that consecutive IRQ_NONE increments unhandled_count. */
static i32 test_irq_nack_count(void)
{
    struct result r =
        request_irq(TEST_IRQ, selftest_irq_handler_none, NULL, "test-none");
    if (r.is_error)
        return 1;

    selftest_irq_call_count = 0;

    /* Dispatch 10 times, all unhandled. */
    for (int i = 0; i < 10; i++)
        irq_dispatch(TEST_IRQ);

    const struct irq_desc *desc = irq_get_desc(TEST_IRQ);
    if (!desc)
        return 1;

    if (desc->unhandled_count != 10)
        return 1;
    if (desc->disabled_nack)
        return 1; /* should not be disabled yet */

    free_irq(TEST_IRQ);
    return 0;
}
DEFINE_SELFTEST(irq_nack_count, test_irq_nack_count);
