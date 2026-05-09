/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit architecture initialization */

#include <isr.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/base.h>

#include <sbi.h>

extern struct result uart_console_init(void);

#if CONFIG_NET_TCP
extern struct result virtio_mmio_net_probe(void);
#endif

/* Called from kernel_init() before any portable subsystem.
 * Brings up the UART and installs the trap vector + PLIC.
 */
void arch_init(void)
{
    assert(!uart_console_init().is_error);
    interrupt_init();

    /* Probe SBI extensions now that UART is up (printk logs results). */
    sbi_init();

    /* Clear SUM and MXR in sstatus: S-mode should not access U-mode pages and
     * execute-only pages should not be implicitly readable.
     */
    __asm__ volatile("csrc sstatus, %0"
                     :
                     : "r"(SSTATUS_SUM | SSTATUS_MXR)
                     : "memory");
}

#if CONFIG_NET_TCP
/* Called from kernel_init() after net_init() to register the NIC driver via the
 * virtio-mmio transport.
 */
struct result arch_net_probe(void)
{
    return virtio_mmio_net_probe();
}
#endif

#if CONFIG_SEMIHOSTING
#include __INC_TEST(arch)
#endif
