/* SPDX-License-Identifier: MIT */
/* RISC-V UART driver - MMIO 16550A on the QEMU virt machine.
 *
 * Provides com_init / com_write for kernel console output.
 * The 'port' argument is ignored; the MMIO base comes from the FDT.
 */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/print.h>

#include "com.h"

#define UART_DEFAULT_BASE 0x10000000UL

static u64 uart_base;

/* 16550A register offsets (byte-wide MMIO). */
#define UART_THR 0 /* Transmit Holding Register (W) */
#define UART_IER 1 /* Interrupt Enable Register */
#define UART_FCR 2 /* FIFO Control Register      (W) */
#define UART_LCR 3 /* Line Control Register */
#define UART_LSR 5 /* Line Status Register */

#define UART_LSR_TX_READY BIT(5) /* THR empty, safe to write */

#define UART_LCR_8N1 0x03  /* 8 data bits, no parity, 1 stop bit */
#define UART_FCR_FIFO 0x01 /* Enable FIFO */

static inline void uart_out(u32 reg, u8 val)
{
    mmio_write8(uart_base + reg, val);
}

static inline u8 uart_in(u32 reg)
{
    return mmio_read8(uart_base + reg);
}

struct result com_init(u16 port __unused)
{
    /* Resolve UART base from FDT, fall back to QEMU-virt default. */
    uart_base = board_info.uart_base ? board_info.uart_base : UART_DEFAULT_BASE;

    /* OpenSBI / QEMU already configures the UART, but re-apply 8N1
     * settings and disable interrupts to match the x86 COM driver behavior.
     */
    uart_out(UART_IER, 0x00);          /* no interrupts */
    uart_out(UART_LCR, UART_LCR_8N1);  /* 8N1 framing */
    uart_out(UART_FCR, UART_FCR_FIFO); /* enable FIFO */
    return result_ok();
}

struct result com_write(u16 port __unused, struct str str)
{
    if (!str.dat || str.len <= 0)
        return result_error(EINVAL);

    for (sz i = 0; i < str.len; i++) {
        for (int j = 0; j < 1000; j++) {
            if (uart_in(UART_LSR) & UART_LSR_TX_READY)
                break;
        }
        uart_out(UART_THR, (u8) str.dat[i]);
    }

    return result_ok();
}

/* Hook printk up to the MMIO UART after com_init() resolves the base. */
static struct result uart_console_write(struct str s)
{
    return com_write(0, s);
}

struct result uart_console_init(void)
{
    struct result r = com_init(0);
    if (!r.is_error)
        print_register_console(uart_console_write);
    return r;
}
