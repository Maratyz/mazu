/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit virtio-mmio network driver.
 *
 * Supports both virtio-mmio version 1 (legacy, v0.9.5) and version 2
 * (modern, v1.0).  QEMU's -device virtio-net-device,netdev=net0 presents
 * version 1 by default on the RISC-V virt machine.
 *
 * QEMU virt machine virtio-mmio memory map (8 slots):
 *   slot 0: base 0x10001000, PLIC IRQ 1
 *   ...
 *   slot 7: base 0x10008000, PLIC IRQ 8
 *
 * QEMU assigns devices to the highest available slot; -device
 * virtio-net-device typically appears at slot 7 (0x10008000, IRQ 8).
 */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/irqdesc.h>
#include <mazu/kvalloc.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/send_buf.h>
#include <mazu/paging.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>

#include "virtio_net.h"

/* Virtqueue sizing */

/* Virtio spec §2.7 requires queue sizes to be powers of two.
 * Using a bitmask instead of modulo avoids integer division on every
 * enqueue/dequeue.
 */
#define VIRTQ_SIZE 256
static_assert((VIRTQ_SIZE & (VIRTQ_SIZE - 1)) == 0,
              "VIRTQ_SIZE must be a power of two");

#define VIRTQ_RX_BUF_SIZE 2048
#define VIRTQ_TX_BUF_SIZE 2048
#define VIRTQ_RX 0
#define VIRTQ_TX 1

/* virtio_net_hdr sizes: 10 bytes in legacy mode (no num_buffers field),
 * 12 bytes in modern mode (VIRTIO_F_VERSION_1 negotiated).
 */
#define VIRTQ_HDR_SIZE_LEGACY 10
#define VIRTQ_HDR_SIZE_MODERN 12

/* Data structures */

struct vq {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u16 *avail_ring;
    struct virtq_used_elem *used_ring;
    u16 size, last_used_idx, free_head, queue_idx;
};

struct virtio_mmio_dev {
    u64 base;
    sz hdr_size; /* 10 (legacy) or 12 (modern) */
    struct mac_addr mac_addr;
    struct vq rx;
    struct vq tx;
    spinlock_t tx_lock; /* protects TX free list, avail ring, and drain */
    byte (*rx_bufs)[VIRTQ_RX_BUF_SIZE];
    byte (*tx_bufs)[VIRTQ_TX_BUF_SIZE];
};

/* Virtqueue allocation, version 2 (independent regions). */

static struct result vq_alloc_modern(struct vq *vq, u16 queue_idx, u16 size)
{
    sz desc_sz = (sz) size * sizeof(struct virtq_desc);
    sz avail_sz = sizeof(struct virtq_avail) + (sz) size * sizeof(u16);
    sz used_sz =
        sizeof(struct virtq_used) + (sz) size * sizeof(struct virtq_used_elem);

    struct option_byte_array dm = kvalloc_alloc(desc_sz, 16);
    struct option_byte_array am = kvalloc_alloc(avail_sz, 2);
    struct option_byte_array um = kvalloc_alloc(used_sz, 4);

    if (dm.is_none || am.is_none || um.is_none) {
        if (!dm.is_none)
            kvalloc_free(option_byte_array_checked(dm));
        if (!am.is_none)
            kvalloc_free(option_byte_array_checked(am));
        if (!um.is_none)
            kvalloc_free(option_byte_array_checked(um));
        return result_error(ENOMEM);
    }

    struct byte_array da = option_byte_array_checked(dm);
    struct byte_array aa = option_byte_array_checked(am);
    struct byte_array ua = option_byte_array_checked(um);

    byte_array_set(da, 0);
    byte_array_set(aa, 0);
    byte_array_set(ua, 0);

    vq->desc = byte_array_ptr(da);
    vq->avail = byte_array_ptr(aa);
    vq->used = byte_array_ptr(ua);
    vq->avail_ring = (u16 *) ((byte *) vq->avail + sizeof(struct virtq_avail));
    vq->used_ring = (struct virtq_used_elem *) ((byte *) vq->used +
                                                sizeof(struct virtq_used));
    vq->size = size;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->queue_idx = queue_idx;

    return result_ok();
}

/* Virtqueue allocation - version 1 legacy (single contiguous region)
 *
 * Layout (virtio v0.9.5):
 *   [0]                       descriptor table  (size × 16 bytes)
 *   [desc_bytes]              available ring    (6 + size × 2 bytes)
 *   [ALIGN_UP(above, 4096)]   used ring         (6 + size × 8 bytes)
 */

static struct result vq_alloc_legacy(struct vq *vq, u16 queue_idx, u16 size)
{
    struct vmio_vq_ptrs ptrs;
    struct result res = vmio_vq_alloc_legacy(size, &ptrs);
    if (res.is_error)
        return res;

    vq->desc = ptrs.desc;
    vq->avail = ptrs.avail;
    vq->used = ptrs.used;
    vq->avail_ring = ptrs.avail_ring;
    vq->used_ring = ptrs.used_ring;
    vq->size = size;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->queue_idx = queue_idx;

    return result_ok();
}

static void vq_init_tx_free_list(struct vq *vq)
{
    for (u16 i = 0; i < vq->size - 1; i++)
        vq->desc[i].next = i + 1;
    vq->desc[vq->size - 1].next = 0xffff;
    vq->free_head = 0;
}

/* Queue setup: write queue rings to device registers */

static struct result vmio_setup_queue_v2(u64 base, struct vq *vq)
{
    mmio_write32(base + VMIO_QUEUE_SEL, vq->queue_idx);

    u16 max_size = (u16) mmio_read32(base + VMIO_QUEUE_NUM_MAX);
    if (max_size == 0)
        return result_error(ENODEV);
    u16 size = (max_size < VIRTQ_SIZE) ? max_size : VIRTQ_SIZE;
    /* Virtio spec §2.7 requires power-of-two queue sizes. */
    if ((size & (size - 1)) != 0) {
        printk(KERN_ERR,
               STR("virtio-mmio: device queue size %u is not a power of "
                   "two\n"),
               (u64) size);
        return result_error(ENODEV);
    }

    struct result res = vq_alloc_modern(vq, vq->queue_idx, size);
    if (res.is_error)
        return res;

    mmio_write32(base + VMIO_QUEUE_NUM, size);

    struct result_paddr_t desc_r = virt_to_phys((vaddr_t) vq->desc);
    struct result_paddr_t avail_r = virt_to_phys((vaddr_t) vq->avail);
    struct result_paddr_t used_r = virt_to_phys((vaddr_t) vq->used);

    if (desc_r.is_error || avail_r.is_error || used_r.is_error)
        return result_error(EINVAL);

    u64 dp = (u64) result_paddr_t_checked(desc_r);
    u64 ap = (u64) result_paddr_t_checked(avail_r);
    u64 up = (u64) result_paddr_t_checked(used_r);

    vmio_write_queue_addr(base, VMIO_QUEUE_DESC_LO, dp);
    vmio_write_queue_addr(base, VMIO_QUEUE_AVAIL_LO, ap);
    vmio_write_queue_addr(base, VMIO_QUEUE_USED_LO, up);
    mmio_write32(base + VMIO_QUEUE_READY, 1);

    return result_ok();
}

static struct result vmio_setup_queue_v1(u64 base, struct vq *vq)
{
    mmio_write32(base + VMIO_QUEUE_SEL, vq->queue_idx);

    u16 max_size = (u16) mmio_read32(base + VMIO_QUEUE_NUM_MAX);
    if (max_size == 0)
        return result_error(ENODEV);
    u16 size = (max_size < VIRTQ_SIZE) ? max_size : VIRTQ_SIZE;
    /* Virtio spec §2.7 requires power-of-two queue sizes. */
    if ((size & (size - 1)) != 0) {
        printk(KERN_ERR,
               STR("virtio-mmio: device queue size %u is not a power of "
                   "two\n"),
               (u64) size);
        return result_error(ENODEV);
    }

    struct result res = vq_alloc_legacy(vq, vq->queue_idx, size);
    if (res.is_error)
        return res;

    mmio_write32(base + VMIO_QUEUE_NUM, size);
    mmio_write32(base + VMIO_QUEUE_ALIGN, (u32) VMIO_PAGE_SIZE);

    /* PFN = physical address of the descriptor table / guest page size. */
    struct result_paddr_t paddr_r = virt_to_phys((vaddr_t) vq->desc);
    if (paddr_r.is_error)
        return result_error(EINVAL);

    paddr_t paddr = result_paddr_t_checked(paddr_r);
    assert(IS_ALIGNED((sz) paddr, (sz) VMIO_PAGE_SIZE));
    mmio_write32(base + VMIO_QUEUE_PFN, (u32) ((u64) paddr / VMIO_PAGE_SIZE));

    return result_ok();
}

static struct result vmio_setup_queue(u64 base, u32 version, struct vq *vq)
{
    if (version == VMIO_VERSION_MODERN)
        return vmio_setup_queue_v2(base, vq);
    return vmio_setup_queue_v1(base, vq);
}

/* vmio_reset() is now in virtio_net.h */

/* RX queue pre-population */

static struct result virtio_mmio_populate_rx(struct virtio_mmio_dev *dev)
{
    struct vq *vq = &dev->rx;

    for (u16 i = 0; i < vq->size; i++) {
        struct result_paddr_t pr = virt_to_phys((vaddr_t) dev->rx_bufs[i]);
        if (pr.is_error)
            return result_error(pr.code);

        vq->desc[i].addr = (u64) result_paddr_t_checked(pr);
        vq->desc[i].len = VIRTQ_RX_BUF_SIZE;
        vq->desc[i].flags = VIRTQ_DESC_F_WRITE;
        vq->desc[i].next = 0;
        vq->avail_ring[i] = i;
    }

    virtio_wmb();
    vq->avail->idx = vq->size;
    virtio_kick();
    mmio_write32(dev->base + VMIO_QUEUE_NOTIFY, VIRTQ_RX);

    return result_ok();
}

/* TX path */

static void vq_tx_drain(struct virtio_mmio_dev *dev)
{
    struct vq *vq = &dev->tx;

    virtio_rmb();
    while (vq->last_used_idx != vq->used->idx) {
        u16 pos = vq->last_used_idx & (vq->size - 1);
        u16 id = (u16) vq->used_ring[pos].id;
        vq->last_used_idx++;

        if (id >= vq->size)
            continue;

        vq->desc[id].next = vq->free_head;
        vq->free_head = id;
    }
}

static struct result virtio_mmio_send_frame(struct netdev *netdev,
                                            struct send_buf sb)
{
    assert(netdev);
    assert(netdev->private_data);
    struct virtio_mmio_dev *dev = netdev->private_data;
    struct vq *vq = &dev->tx;

    u64 flags = spin_lock_irqsave(&dev->tx_lock);

    vq_tx_drain(dev);

    if (vq->free_head == 0xffff) {
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return result_error(ENOBUFS);
    }

    sz pkt_len = send_buf_total_length(sb);
    sz total_len = dev->hdr_size + pkt_len;
    if (total_len > VIRTQ_TX_BUF_SIZE) {
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return result_error(EINVAL);
    }

    u16 desc_idx = vq->free_head;
    vq->free_head = vq->desc[desc_idx].next;

    /* Build: zero virtio_net_hdr (hdr_size bytes) followed by payload. */
    byte *tx_buf = dev->tx_bufs[desc_idx];
    byte_array_set(byte_array_new(tx_buf, dev->hdr_size), 0);

    struct byte_buf payload = byte_buf_new(tx_buf + dev->hdr_size, 0, pkt_len);
    struct result res = send_buf_assemble(sb, &payload);
    if (res.is_error) {
        vq->desc[desc_idx].next = vq->free_head;
        vq->free_head = desc_idx;
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return res;
    }

    struct result_paddr_t paddr_r = virt_to_phys((vaddr_t) tx_buf);
    if (paddr_r.is_error) {
        vq->desc[desc_idx].next = vq->free_head;
        vq->free_head = desc_idx;
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return result_error(paddr_r.code);
    }

    vq->desc[desc_idx].addr = (u64) result_paddr_t_checked(paddr_r);
    vq->desc[desc_idx].len = (u32) total_len;
    vq->desc[desc_idx].flags = 0;
    vq->desc[desc_idx].next = 0;

    u16 avail_pos = vq->avail->idx & (vq->size - 1);
    vq->avail_ring[avail_pos] = desc_idx;

    virtio_wmb();
    vq->avail->idx++;
    virtio_kick();
    mmio_write32(dev->base + VMIO_QUEUE_NOTIFY, VIRTQ_TX);

    spin_unlock_irqrestore(&dev->tx_lock, flags);
    return result_ok();
}

/* RX path */

static void vq_rx_drain(struct virtio_mmio_dev *dev, struct netdev *netdev)
{
    struct vq *vq = &dev->rx;
    u16 start_idx = vq->avail->idx;

    virtio_rmb();

    while (vq->last_used_idx != vq->used->idx) {
        u16 pos = vq->last_used_idx & (vq->size - 1);
        u16 desc_idx = (u16) vq->used_ring[pos].id;
        u32 written = vq->used_ring[pos].len;
        vq->last_used_idx++;

        if (desc_idx >= vq->size)
            continue;
        if (written > VIRTQ_RX_BUF_SIZE)
            written = VIRTQ_RX_BUF_SIZE;

        if ((sz) written > dev->hdr_size) {
            byte *buf = dev->rx_bufs[desc_idx];
            netdev_intr_receive(netdev,
                                byte_view_new(buf + dev->hdr_size,
                                              (sz) written - dev->hdr_size));
        }

        vq->avail_ring[vq->avail->idx & (vq->size - 1)] = desc_idx;
        virtio_wmb();
        vq->avail->idx++;
    }

    if (vq->avail->idx != start_idx) {
        virtio_kick();
        mmio_write32(dev->base + VMIO_QUEUE_NOTIFY, VIRTQ_RX);
    }
}

/* Interrupt handler */

static int virtio_mmio_isr(int irq __unused, void *arg)
{
    assert(arg);
    struct netdev *netdev = arg;
    struct virtio_mmio_dev *dev = netdev->private_data;

    u32 isr = mmio_read32(dev->base + VMIO_INT_STATUS);
    if (!isr)
        return IRQ_NONE;
    mmio_write32(dev->base + VMIO_INT_ACK, isr);

    if (!(isr & VIRTIO_ISR_QUEUE_INT))
        return IRQ_HANDLED; /* Configuration change interrupt. */

    spin_lock(&dev->tx_lock);
    vq_tx_drain(dev);
    spin_unlock(&dev->tx_lock);

    vq_rx_drain(dev, netdev);

    return IRQ_HANDLED;
}

/* Probe a single MMIO slot */

static struct result vmio_probe_slot(u64 base, u32 irq)
{
    u32 magic = mmio_read32(base + VMIO_MAGIC);
    u32 version = mmio_read32(base + VMIO_VERSION);
    u32 dev_id = mmio_read32(base + VMIO_DEVICE_ID);

    if (magic != (u32) VMIO_MAGIC_VALUE)
        return result_error(ENODEV);
    if (version != VMIO_VERSION_LEGACY && version != VMIO_VERSION_MODERN)
        return result_error(ENODEV);
    if (dev_id != VMIO_DEVICE_ID_NET)
        return result_error(ENODEV);

    /* Allocate device struct and I/O buffers. */
    struct option_byte_array dev_m = kvalloc_alloc(
        sizeof(struct virtio_mmio_dev), alignof(struct virtio_mmio_dev));
    if (dev_m.is_none)
        return result_error(ENOMEM);
    struct byte_array dev_a = option_byte_array_checked(dev_m);
    byte_array_set(dev_a, 0);
    struct virtio_mmio_dev *dev = byte_array_ptr(dev_a);

    struct result res;

    dev->base = base;
    dev->hdr_size = (version == VMIO_VERSION_MODERN) ? VIRTQ_HDR_SIZE_MODERN
                                                     : VIRTQ_HDR_SIZE_LEGACY;

    struct option_byte_array rx_m =
        kvalloc_alloc((sz) VIRTQ_SIZE * VIRTQ_RX_BUF_SIZE, 64);
    struct option_byte_array tx_m =
        kvalloc_alloc((sz) VIRTQ_SIZE * VIRTQ_TX_BUF_SIZE, 64);
    struct option_byte_array nd_m =
        kvalloc_alloc(sizeof(struct netdev), alignof(struct netdev));
    if (rx_m.is_none || tx_m.is_none || nd_m.is_none) {
        res = result_error(ENOMEM);
        goto err_free_bufs;
    }
    struct byte_array rx_a = option_byte_array_checked(rx_m);
    struct byte_array tx_a = option_byte_array_checked(tx_m);
    byte_array_set(rx_a, 0);
    byte_array_set(tx_a, 0);
    dev->rx_bufs = byte_array_ptr(rx_a);
    dev->tx_bufs = byte_array_ptr(tx_a);
    struct netdev *netdev = byte_array_ptr(option_byte_array_checked(nd_m));

    /* Virtio initialization sequence. */

    /* Step 1: reset. */
    vmio_reset(base);

    /* Step 2-3: ACKNOWLEDGE + DRIVER. */
    vmio_set_driver(base);

    /* Step 4: feature negotiation. */
    if (version == VMIO_VERSION_MODERN) {
        /* Two-word feature negotiation (v2). */
        mmio_write32(base + VMIO_DEV_FEAT_SEL, 0);
        u32 feat0 = mmio_read32(base + VMIO_DEV_FEAT);
        mmio_write32(base + VMIO_DEV_FEAT_SEL, 1);
        u32 feat1 = mmio_read32(base + VMIO_DEV_FEAT);

        if (!(feat0 & VIRTIO_NET_F_MAC) || !(feat1 & VIRTIO_F_VERSION_1)) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(ENODEV);
            goto err_free_bufs;
        }

        mmio_write32(base + VMIO_DRV_FEAT_SEL, 0);
        mmio_write32(base + VMIO_DRV_FEAT, VIRTIO_NET_F_MAC);
        mmio_write32(base + VMIO_DRV_FEAT_SEL, 1);
        mmio_write32(base + VMIO_DRV_FEAT, VIRTIO_F_VERSION_1);

        /* Step 5: FEATURES_OK (v2 only). */
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                             VIRTIO_STATUS_DRIVER |
                                             VIRTIO_STATUS_FEATURES_OK);
        if (!(mmio_read32(base + VMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(ENODEV);
            goto err_free_bufs;
        }
    } else {
        /* Single-word feature negotiation for version 1 devices. */
        mmio_write32(base + VMIO_DEV_FEAT_SEL, 0);
        u32 feat0 = mmio_read32(base + VMIO_DEV_FEAT);
        if (!(feat0 & VIRTIO_NET_F_MAC)) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(ENODEV);
            goto err_free_bufs;
        }

        mmio_write32(base + VMIO_DRV_FEAT_SEL, 0);
        mmio_write32(base + VMIO_DRV_FEAT, VIRTIO_NET_F_MAC);

        /* Version 1 devices require the guest page size before queue setup. */
        mmio_write32(base + VMIO_GUEST_PAGE_SZ, (u32) VMIO_PAGE_SIZE);
    }

    /* Step 6: read MAC from device config space. */
    for (sz i = 0; i < 6; i++)
        dev->mac_addr.addr[i] = mmio_read8(base + VMIO_CONFIG + i);

    /* Step 7: configure virtqueues. */
    dev->rx.queue_idx = VIRTQ_RX;
    dev->tx.queue_idx = VIRTQ_TX;

    res = vmio_setup_queue(base, version, &dev->rx);
    if (res.is_error) {
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
        goto err_free_bufs;
    }
    res = vmio_setup_queue(base, version, &dev->tx);
    if (res.is_error) {
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
        goto err_free_bufs;
    }

    vq_init_tx_free_list(&dev->tx);

    /* Step 8: pre-populate RX descriptors. */
    res = virtio_mmio_populate_rx(dev);
    if (res.is_error) {
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
        goto err_free_bufs;
    }

    /* Step 9: set DRIVER_OK. The device may now access the posted buffers. */
    mmio_write32(
        base + VMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            (version == VMIO_VERSION_MODERN ? VIRTIO_STATUS_FEATURES_OK : 0) |
            VIRTIO_STATUS_DRIVER_OK);

    /* Log. */
    static byte mac_fmt[MAC_ADDR_FMT_BUF_SIZE];
    struct arena mac_arn = arena_new(byte_array_new(mac_fmt, countof(mac_fmt)));
    printk(KERN_INFO, STR("virtio-mmio net v%u: MAC %s (MMIO=0x%lx IRQ=%u)\n"),
           (u64) version, mac_addr_format(dev->mac_addr, &mac_arn), base,
           (u64) irq);

    /* Publish the device to the network stack before wiring up interrupts. */
    netdev->mac_addr = dev->mac_addr;
    netdev->ip_addr = IPV4_ADDR_ANY;
    netdev->link_type = NETDEV_LINK_TYPE_ETHERNET;
    netdev->send_frame = virtio_mmio_send_frame;
    netdev->mtu = VIRTQ_TX_BUF_SIZE - dev->hdr_size;
    netdev->private_data = dev;

    res = netdev_register_device(netdev);
    if (res.is_error)
        goto err_reset_dev;

    res = request_irq((int) irq, virtio_mmio_isr, netdev, "virtio-net");
    if (res.is_error)
        goto err_unregister_netdev;

    printk(KERN_INFO, STR("virtio-mmio net: ready\n"));
    return result_ok();

err_unregister_netdev:
    netdev_unregister_device(netdev);
err_reset_dev:
    vmio_reset(base);
err_free_bufs:
    if (!rx_m.is_none)
        kvalloc_free(option_byte_array_checked(rx_m));
    if (!tx_m.is_none)
        kvalloc_free(option_byte_array_checked(tx_m));
    if (!nd_m.is_none)
        kvalloc_free(option_byte_array_checked(nd_m));
    kvalloc_free(dev_a);
    return res;
}

/* Public entry point called from arch_net_probe() in arch.c. */

struct result virtio_mmio_net_probe(void)
{
    return vmio_scan_slots(vmio_probe_slot);
}
