/* SPDX-License-Identifier: MIT */
/* Flattened Device Tree (FDT) parser.
 *
 * Minimal read-only in-place FDT walker that extracts board-level info (PLIC,
 * UART, VirtIO, CLINT, memory, timebase) from the DTB passed by OpenSBI in a1.
 * Falls back to QEMU-virt defaults when parsing fails.
 */

#ifndef MAZU_FDT_H
#define MAZU_FDT_H

#include <mazu/base.h>

#define FDT_MAX_VIRTIO_SLOTS 8

struct fdt_board_info {
    u64 dram_base;
    u64 dram_size;
    u64 uart_base;
    u64 plic_base;
    u32 plic_nr_sources;
    u64 clint_base;
    u64 virtio_base[FDT_MAX_VIRTIO_SLOTS];
    u32 virtio_irq[FDT_MAX_VIRTIO_SLOTS];
    u32 virtio_count;
    u32 nr_harts;
    u64 timebase_freq;
};

/* Parse the FDT at fdt_addr. Fills 'info' with discovered values; unrecognized
 * fields are left at zero.
 * Returns 0 on success, -1 on invalid header or unsupported version.
 */
int fdt_parse(const void *fdt_addr, struct fdt_board_info *info);

/* Global board info populated during early boot. */
extern struct fdt_board_info board_info;

/* FDT physical address saved by _start before BSS zeroing. */
extern u64 _fdt_addr;

#endif /* MAZU_FDT_H */
