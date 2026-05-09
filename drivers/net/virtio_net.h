/* SPDX-License-Identifier: MIT */
#ifndef MAZU_NET_VIRTIO_NET_H
#define MAZU_NET_VIRTIO_NET_H

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/barrier.h>
#include <mazu/base.h>
#include <mazu/kvalloc.h>

/* virtio-mmio register offsets (common to v1 and v2) */

#define VMIO_MAGIC 0x000
#define VMIO_VERSION 0x004
#define VMIO_DEVICE_ID 0x008
#define VMIO_DEV_FEAT 0x010
#define VMIO_DEV_FEAT_SEL 0x014
#define VMIO_DRV_FEAT 0x020
#define VMIO_DRV_FEAT_SEL 0x024
#define VMIO_GUEST_PAGE_SZ 0x028 /* v1 only */
#define VMIO_QUEUE_SEL 0x030
#define VMIO_QUEUE_NUM_MAX 0x034
#define VMIO_QUEUE_NUM 0x038
#define VMIO_QUEUE_ALIGN 0x03c /* v1 only */
#define VMIO_QUEUE_PFN 0x040   /* v1 only */
#define VMIO_QUEUE_READY 0x044 /* v2 only */
#define VMIO_QUEUE_NOTIFY 0x050
#define VMIO_INT_STATUS 0x060
#define VMIO_INT_ACK 0x064
#define VMIO_STATUS 0x070
#define VMIO_QUEUE_DESC_LO 0x080  /* v2 only */
#define VMIO_QUEUE_DESC_HI 0x084  /* v2 only */
#define VMIO_QUEUE_AVAIL_LO 0x090 /* v2 only */
#define VMIO_QUEUE_AVAIL_HI 0x094 /* v2 only */
#define VMIO_QUEUE_USED_LO 0x0a0  /* v2 only */
#define VMIO_QUEUE_USED_HI 0x0a4  /* v2 only */
#define VMIO_CONFIG 0x100

#define VMIO_MAGIC_VALUE 0x74726976UL
#define VMIO_VERSION_LEGACY 1
#define VMIO_VERSION_MODERN 2
#define VMIO_DEVICE_ID_NET 1
#define VMIO_DEVICE_ID_BLK 2

#define VMIO_PAGE_SIZE 4096UL

/* QEMU virt machine: 8 slots, stride 0x1000, base 0x10001000, IRQ base 1. */
#define VMIO_BASE_ADDR 0x10001000UL
#define VMIO_SLOT_STRIDE 0x1000UL
#define VMIO_NUM_SLOTS 8
#define VMIO_IRQ_BASE 1

/* Virtqueue memory fences -- use generic barrier taxonomy from barrier.h.
 *
 * dma_wmb()  - descriptor writes visible before bumping avail->idx.
 * io_wmb()   - DRAM writes visible before MMIO doorbell store.
 * dma_rmb()  - used->idx read ordered before used_ring element reads.
 *
 * Legacy aliases retained for grep-ability; prefer the generic names.
 */
#define virtio_wmb() dma_wmb()
#define virtio_kick() io_wmb()
#define virtio_rmb() dma_rmb()

/* Device status bits */

#define VIRTIO_STATUS_RESET 0
#define VIRTIO_STATUS_ACKNOWLEDGE BIT(0)
#define VIRTIO_STATUS_DRIVER BIT(1)
#define VIRTIO_STATUS_DRIVER_OK BIT(2)
#define VIRTIO_STATUS_FEATURES_OK BIT(3)
#define VIRTIO_STATUS_FAILED BIT(7)

/* Shared initialization helpers */

/* Reset a virtio-mmio device and wait until the status register clears. */
static inline void vmio_reset(u64 base)
{
    mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_RESET);
    while (mmio_read32(base + VMIO_STATUS) != 0)
        ;
}

/* Set ACKNOWLEDGE | DRIVER status bits (steps 2-3 of virtio init). */
static inline void vmio_set_driver(u64 base)
{
    mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(base + VMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
}

/* Write the 64-bit physical address of a v2 queue region to MMIO regs. */
static inline void vmio_write_queue_addr(u64 base, u32 lo_reg, u64 paddr)
{
    mmio_write32(base + lo_reg, (u32) (paddr & 0xffffffffUL));
    mmio_write32(base + lo_reg + 4, (u32) (paddr >> 32));
}

/* Feature bits */

/* Word 0 (device_feature_select = 0) */
#define VIRTIO_NET_F_MAC BIT(5)

/* Word 1 (device_feature_select = 1); bit 32 of the full feature set */
#define VIRTIO_F_VERSION_1 BIT(0) /* word-1 bit 0 = overall bit 32 */

/* Virtqueue descriptor flags */

#define VIRTQ_DESC_F_NEXT BIT(0)  /* descriptor chain continues */
#define VIRTQ_DESC_F_WRITE BIT(1) /* device-writable (RX buffers) */

/* ISR register bits */

#define VIRTIO_ISR_QUEUE_INT BIT(0) /* virtqueue interrupt */

/* virtio_net_hdr (12 bytes - VirtIO 1.0 modern always includes num_buffers)*/

struct virtio_net_hdr {
    u8 flags;
    u8 gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
    u16 num_buffers; /* always present when VIRTIO_F_VERSION_1 is negotiated */
} __packed;

static_assert(sizeof(struct virtio_net_hdr) == 12,
              "unexpected virtio_net_hdr size");

/* Virtqueue descriptor */

struct virtq_desc {
    u64 addr; /* physical address */
    u32 len;
    u16 flags;
    u16 next;
} __packed;

static_assert(sizeof(struct virtq_desc) == 16, "unexpected virtq_desc size");

/* Virtqueue available ring */

/* Only the fixed header; ring[] is accessed via pointer arithmetic. */
struct virtq_avail {
    u16 flags;
    u16 idx;
    /* u16 ring[queue_size] follows */
} __packed;

/* Virtqueue used ring */

struct virtq_used_elem {
    u32 id;  /* descriptor chain head index */
    u32 len; /* bytes written by device */
} __packed;

struct virtq_used {
    u16 flags;
    u16 idx;
    /* struct virtq_used_elem ring[queue_size] follows */
} __packed;

/* Shared legacy (v1) virtqueue allocation.
 *
 * Layout (virtio v0.9.5):
 *   [0]                       descriptor table  (size x 16 bytes)
 *   [desc_bytes]              available ring    (4 + size x 2 bytes)
 *   [ALIGN_UP(above, 4096)]   used ring         (4 + size x 8 bytes)
 *
 * Caller sets:
 *   out_desc, out_avail, out_used, out_avail_ring, out_used_ring
 * from the returned pointers.
 */
struct vmio_vq_ptrs {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u16 *avail_ring;
    struct virtq_used_elem *used_ring;
};

static inline struct result vmio_vq_alloc_legacy(u16 size,
                                                 struct vmio_vq_ptrs *out)
{
    sz desc_bytes = (sz) size * sizeof(struct virtq_desc);
    sz avail_bytes = sizeof(struct virtq_avail) + (sz) size * sizeof(u16);
    sz used_off = ALIGN_UP(desc_bytes + avail_bytes, (sz) VMIO_PAGE_SIZE);
    sz used_bytes =
        sizeof(struct virtq_used) + (sz) size * sizeof(struct virtq_used_elem);
    sz total = used_off + used_bytes;

    struct option_byte_array mem_opt =
        kvalloc_alloc(total, (sz) VMIO_PAGE_SIZE);
    if (mem_opt.is_none)
        return result_error(ENOMEM);

    struct byte_array mem = option_byte_array_checked(mem_opt);
    byte_array_set(mem, 0);
    byte *base = byte_array_ptr(mem);

    out->desc = (struct virtq_desc *) base;
    out->avail = (struct virtq_avail *) (base + desc_bytes);
    out->used = (struct virtq_used *) (base + used_off);
    out->avail_ring =
        (u16 *) ((byte *) out->avail + sizeof(struct virtq_avail));
    out->used_ring = (struct virtq_used_elem *) ((byte *) out->used +
                                                 sizeof(struct virtq_used));

    return result_ok();
}

/* Iterate FDT-discovered slots, then fall back to hardcoded QEMU-virt scan.
 * Calls probe_fn(base, irq) for each candidate; stops on the first success.
 */
typedef struct result (*vmio_probe_fn_t)(u64 base, u32 irq);

static inline struct result vmio_scan_slots(vmio_probe_fn_t probe_fn)
{
    /* Try FDT-discovered virtio-mmio slots first. */
    for (u32 i = 0; i < board_info.virtio_count; i++) {
        struct result res =
            probe_fn(board_info.virtio_base[i], board_info.virtio_irq[i]);
        if (!res.is_error)
            return result_ok();
    }

    /* Fall back to hardcoded QEMU-virt slot scan. */
    for (sz i = 0; i < VMIO_NUM_SLOTS; i++) {
        u64 base = VMIO_BASE_ADDR + (u64) i * VMIO_SLOT_STRIDE;
        u32 irq = VMIO_IRQ_BASE + (u32) i;
        struct result res = probe_fn(base, irq);
        if (!res.is_error)
            return result_ok();
    }

    return result_error(ENODEV);
}

#endif /* MAZU_NET_VIRTIO_NET_H */
