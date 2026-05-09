/* SPDX-License-Identifier: MIT */

/* RISC-V 64-bit virtio-mmio block driver.
 *
 * Supports both virtio-mmio version 1 (legacy) and version 2 (modern).
 * Uses a single requestq with synchronous and asynchronous I/O paths.
 * Both submit a three-descriptor chain (header + data + status).
 * Sync: poll until ISR sets io_done.  Async: ISR invokes callback.
 *
 * QEMU virt machine: -drive file=disk.img,format=raw,if=none,id=blk0
 *                    -device virtio-blk-device,drive=blk0
 */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/blkdev.h>
#include <mazu/byte.h>
#include <mazu/irqdesc.h>
#include <mazu/kvalloc.h>
#include <mazu/paging.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>

#include "../net/virtio_net.h"

/* virtio-blk request types. */
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

/* virtio-blk status codes. */
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1

/* Virtqueue sizing. */
#define VBQ_SIZE 16
static_assert((VBQ_SIZE & (VBQ_SIZE - 1)) == 0,
              "VBQ_SIZE must be power of two");

/* Data structures */

struct virtio_blk_req {
    u32 type;
    u32 reserved;
    u64 sector;
} __packed;

struct virtio_blk_dev {
    u64 base;
    u64 capacity; /* in sectors */
    struct {
        struct virtq_desc *desc;
        struct virtq_avail *avail;
        struct virtq_used *used;
        u16 *avail_ring;
        struct virtq_used_elem *used_ring;
        u16 size;
        u16 last_used_idx;
    } vq;
    bool io_done;  /* accessed via __atomic_{load,store} */
    bool inflight; /* accessed via __atomic_{load,store}; true while request in
                      virtqueue
 */
    u8 last_status;
    spinlock_t lock;
    struct virtio_blk_req req; /* DMA-safe request header */
    u8 status_byte;            /* DMA-safe status byte */
    paddr_t req_pa;            /* phys addr of req, cached at probe */
    paddr_t st_pa;             /* phys addr of status_byte, cached at probe */

    /* Async I/O completion callback.  Set before submit, cleared after
     * the ISR invokes it.  NULL = synchronous mode (io_done poll).
     * Accessed via __atomic_{load,store} with release/acquire ordering
     * because the ISR may run on a different hart than the submitter.
     */
    blk_callback_t async_cb;
    void *async_cb_ctx;
};

struct blkdev *global_blkdev;

/* Virtqueue allocation (legacy + modern) */

static struct result vblk_vq_alloc(struct virtio_blk_dev *dev)
{
    struct vmio_vq_ptrs ptrs;
    struct result res = vmio_vq_alloc_legacy(dev->vq.size, &ptrs);
    if (res.is_error)
        return res;

    dev->vq.desc = ptrs.desc;
    dev->vq.avail = ptrs.avail;
    dev->vq.used = ptrs.used;
    dev->vq.avail_ring = ptrs.avail_ring;
    dev->vq.used_ring = ptrs.used_ring;
    dev->vq.last_used_idx = 0;

    return result_ok();
}

/* I/O submission

 * I/O timeout: ~2 seconds at 10 MHz timebase (20 million ticks).
 * QEMU virtio-blk completes in microseconds; this is a safety net.
 */
#define VBLK_IO_TIMEOUT_TICKS 20000000ULL

/* Build and submit a three-descriptor chain (header + data + status).
 * Must be called with dev->lock held.  Sets dev->inflight = true if the slot
 * is free.
 * Returns with the request submitted and the lock still held; caller is
 * responsible for releasing the lock.
 */
static struct result vblk_submit_chain(struct virtio_blk_dev *dev,
                                       u32 type,
                                       u64 sector,
                                       u32 count,
                                       byte *buf)
{
    if (__atomic_load_n(&dev->inflight, __ATOMIC_ACQUIRE))
        return result_error(EBUSY);
    __atomic_store_n(&dev->inflight, true, __ATOMIC_RELAXED);

#define VBLK_SUBMIT_FAIL(code)                                     \
    do {                                                           \
        __atomic_store_n(&dev->inflight, false, __ATOMIC_RELAXED); \
        return result_error(code);                                 \
    } while (0)

    struct result_paddr_t buf_pa = virt_to_phys((vaddr_t) buf);
    if (buf_pa.is_error)
        VBLK_SUBMIT_FAIL(EINVAL);

    if (count > (u32) ~0 / BLKDEV_SECTOR_SIZE)
        VBLK_SUBMIT_FAIL(EINVAL);
    u32 data_len = count * BLKDEV_SECTOR_SIZE;

    dev->req.type = type;
    dev->req.reserved = 0;
    dev->req.sector = sector;
    dev->status_byte = 0xff;

    /* Descriptor 0: request header. */
    dev->vq.desc[0].addr = (u64) dev->req_pa;
    dev->vq.desc[0].len = sizeof(dev->req);
    dev->vq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    dev->vq.desc[0].next = 1;

    /* Descriptor 1: data buffer. */
    dev->vq.desc[1].addr = (u64) result_paddr_t_checked(buf_pa);
    dev->vq.desc[1].len = data_len;
    dev->vq.desc[1].flags =
        VIRTQ_DESC_F_NEXT |
        ((type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0);
    dev->vq.desc[1].next = 2;

    /* Descriptor 2: status byte. */
    dev->vq.desc[2].addr = (u64) dev->st_pa;
    dev->vq.desc[2].len = sizeof(dev->status_byte);
    dev->vq.desc[2].flags = VIRTQ_DESC_F_WRITE;
    dev->vq.desc[2].next = 0;

    /* Clear completion flag before making the request visible to the
     * device.  Otherwise, a fast completion could set io_done = true
     * before the clear, causing the poll loop to spin until timeout.
     */
    __atomic_store_n(&dev->io_done, false, __ATOMIC_RELAXED);

    /* Submit to available ring. */
    u16 avail_idx = dev->vq.avail->idx;
    dev->vq.avail_ring[avail_idx & (dev->vq.size - 1)] = 0;
    virtio_wmb();
    dev->vq.avail->idx = avail_idx + 1;
    virtio_kick();

    mmio_write32(dev->base + VMIO_QUEUE_NOTIFY, 0);

#undef VBLK_SUBMIT_FAIL
    return result_ok();
}

/* Synchronous I/O */

static struct result vblk_do_io(struct virtio_blk_dev *dev,
                                u32 type,
                                u64 sector,
                                u32 count,
                                byte *buf)
{
    /* Serialize the entire submit-wait sequence so two harts cannot
     * interleave requests that share the per-device req/status_byte
     * and descriptor chain.  The lock is held across the wait; this
     * is safe because PLIC interrupts are selectively enabled below
     * and the only ISR that fires (vblk_isr) does not acquire this
     * lock - it only sets io_done.
     */
    u64 spin_flags = spin_lock_irqsave(&dev->lock);

    struct result res = vblk_submit_chain(dev, type, sector, count, buf);
    if (res.is_error) {
        spin_unlock_irqrestore(&dev->lock, spin_flags);
        return res;
    }

    /* Selectively enable external (PLIC) interrupts so the completion
     * ISR can fire, while keeping timer (STIE) and software (SSIE)
     * masked to prevent preemption or IPI processing.
     *
     * Safety: the only ISR that can fire is vblk_isr, which does NOT
     * acquire dev->lock - it only writes io_done and acks the device.
     * So no lock re-entry is possible despite sstatus.SIE being set.
     *
     * Poll instead of wfi: on SMP the PLIC may route the completion
     * interrupt to a different hart, so wfi could block indefinitely.
     * The ISR sets io_done on whichever hart handles the interrupt;
     * the atomic acquire-load here picks it up.
     *
     * Ordering: disable sstatus.SIE first, then snapshot/modify sie,
     * then re-enable sstatus.SIE.  This avoids a window where a trap
     * could modify sie between snapshot and disable.
     */
    u64 wait_sstatus_sie = intr_disable();
    u64 wait_sie;
    __asm__ volatile("csrr %0, sie" : "=r"(wait_sie)::"memory");
    __asm__ volatile("csrc sie, %0"
                     :
                     : "r"((u64) (SIE_SSIE | SIE_STIE))
                     : "memory");
    __asm__ volatile("csrs sie, %0" : : "r"((u64) SIE_SEIE) : "memory");
    __asm__ volatile("csrsi sstatus, 2" ::: "memory");

    u64 deadline = time_rdtime() + VBLK_IO_TIMEOUT_TICKS;
    bool timed_out = false;
    while (!__atomic_load_n(&dev->io_done, __ATOMIC_ACQUIRE)) {
        if (time_rdtime() > deadline) {
            timed_out = true;
            break;
        }
    }

    __asm__ volatile("csrci sstatus, 2" ::: "memory");
    __asm__ volatile("csrw sie, %0" : : "r"(wait_sie) : "memory");
    intr_restore(wait_sstatus_sie);

    /* Capture status under the lock before another request can overwrite it. */
    u8 io_status = dev->status_byte;
    dev->last_status = io_status;

    __atomic_store_n(&dev->inflight, false, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&dev->lock, spin_flags);

    if (timed_out) {
        printk(KERN_ERR, STR("virtio-blk: I/O timeout (sector %lu)\n"),
               (unsigned long) sector);
        return result_error(EIO);
    }

    if (io_status != VIRTIO_BLK_S_OK)
        return result_error(EIO);

    return result_ok();
}

/* Async I/O: submit request and return immediately.  The ISR calls cb
 * on completion.  Caller must ensure buf remains valid until callback.
 * dev->lock serializes with synchronous I/O and other async submits.
 */
static struct result vblk_do_io_async(struct virtio_blk_dev *dev,
                                      u32 type,
                                      u64 sector,
                                      u32 count,
                                      byte *buf,
                                      blk_callback_t cb,
                                      void *cb_ctx)
{
    if (!cb)
        return result_error(EINVAL);

    u64 spin_flags = spin_lock_irqsave(&dev->lock);

    /* Check inflight before publishing callback - if another request is
     * already in flight, its async_cb must not be overwritten.
     */
    if (__atomic_load_n(&dev->inflight, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&dev->lock, spin_flags);
        return result_error(EBUSY);
    }

    /* Register async callback before submit - ISR may fire on another
     * hart immediately after notify.  Store cb_ctx first, then release-
     * store cb so the ISR's acquire-load sees both fields consistently.
     * Note: the callback may fire before this function returns.
     */
    dev->async_cb_ctx = cb_ctx;
    __atomic_store_n(&dev->async_cb, cb, __ATOMIC_RELEASE);

    struct result res = vblk_submit_chain(dev, type, sector, count, buf);
    if (res.is_error) {
        __atomic_store_n(&dev->async_cb, NULL, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&dev->lock, spin_flags);
        return res;
    }

    spin_unlock_irqrestore(&dev->lock, spin_flags);

    return result_ok();
}

/* blkdev interface */

static inline bool vblk_range_ok(struct blkdev *bdev, u64 sector, u32 count)
{
    u64 end = sector + (u64) count;
    return count > 0 && !ADD_OVERFLOW(sector, (u64) count) &&
           end <= bdev->capacity;
}

static struct result vblk_read_sectors(struct blkdev *bdev,
                                       u64 sector,
                                       u32 count,
                                       byte *buf)
{
    if (!vblk_range_ok(bdev, sector, count))
        return result_error(EINVAL);
    struct virtio_blk_dev *dev = bdev->private_data;
    return vblk_do_io(dev, VIRTIO_BLK_T_IN, sector, count, buf);
}

static struct result vblk_write_sectors(struct blkdev *bdev,
                                        u64 sector,
                                        u32 count,
                                        const byte *buf)
{
    if (!vblk_range_ok(bdev, sector, count))
        return result_error(EINVAL);
    struct virtio_blk_dev *dev = bdev->private_data;
    return vblk_do_io(dev, VIRTIO_BLK_T_OUT, sector, count, (byte *) buf);
}

static struct result vblk_read_sectors_async(struct blkdev *bdev,
                                             u64 sector,
                                             u32 count,
                                             byte *buf,
                                             blk_callback_t cb,
                                             void *cb_ctx)
{
    if (!vblk_range_ok(bdev, sector, count))
        return result_error(EINVAL);
    struct virtio_blk_dev *dev = bdev->private_data;
    return vblk_do_io_async(dev, VIRTIO_BLK_T_IN, sector, count, buf, cb,
                            cb_ctx);
}

static struct result vblk_write_sectors_async(struct blkdev *bdev,
                                              u64 sector,
                                              u32 count,
                                              const byte *buf,
                                              blk_callback_t cb,
                                              void *cb_ctx)
{
    if (!vblk_range_ok(bdev, sector, count))
        return result_error(EINVAL);
    struct virtio_blk_dev *dev = bdev->private_data;
    return vblk_do_io_async(dev, VIRTIO_BLK_T_OUT, sector, count, (byte *) buf,
                            cb, cb_ctx);
}

/* Interrupt handler */

static int vblk_isr(int irq __unused, void *arg)
{
    struct virtio_blk_dev *dev = arg;
    u32 isr = mmio_read32(dev->base + VMIO_INT_STATUS);
    if (!isr)
        return IRQ_NONE;
    mmio_write32(dev->base + VMIO_INT_ACK, isr);

    /* Mark used entries consumed.  The read barrier must come after
     * reading used->idx so that subsequent reads of descriptor data
     * (e.g. status_byte) see the device's writes.
     */
    if (dev->vq.last_used_idx != dev->vq.used->idx) {
        virtio_rmb();
        dev->vq.last_used_idx = dev->vq.used->idx;

        /* Check for async callback.  Acquire-load pairs with the
         * release-store in vblk_do_io_async to ensure cross-hart
         * visibility of async_cb_ctx and descriptor state.
         */
        blk_callback_t cb = __atomic_load_n(&dev->async_cb, __ATOMIC_ACQUIRE);
        if (cb) {
            /* Async path: clear inflight before callback so the
             * callback can legally resubmit to this device.
             */
            void *cb_ctx = dev->async_cb_ctx;
            __atomic_store_n(&dev->async_cb, NULL, __ATOMIC_RELAXED);
            __atomic_store_n(&dev->inflight, false, __ATOMIC_RELEASE);
            int status = (dev->status_byte == VIRTIO_BLK_S_OK) ? 0 : -EIO;
            cb(cb_ctx, status);
        } else {
            /* Sync path: release-store pairs with acquire-load in
             * the poll loop.  inflight is cleared by the poll caller
             * under dev->lock.
             */
            __atomic_store_n(&dev->io_done, true, __ATOMIC_RELEASE);
        }
    }
    return IRQ_HANDLED;
}

/* Probe */

static struct result vblk_probe_slot(u64 base, u32 irq)
{
    u32 magic = mmio_read32(base + VMIO_MAGIC);
    u32 version = mmio_read32(base + VMIO_VERSION);
    u32 dev_id = mmio_read32(base + VMIO_DEVICE_ID);

    if (magic != (u32) VMIO_MAGIC_VALUE)
        return result_error(ENODEV);
    if (version != VMIO_VERSION_LEGACY && version != VMIO_VERSION_MODERN)
        return result_error(ENODEV);
    if (dev_id != VMIO_DEVICE_ID_BLK)
        return result_error(ENODEV);

    /* Allocate device struct. */
    struct option_byte_array dev_m = kvalloc_alloc(
        sizeof(struct virtio_blk_dev), alignof(struct virtio_blk_dev));
    if (dev_m.is_none)
        return result_error(ENOMEM);
    struct byte_array dev_a = option_byte_array_checked(dev_m);
    byte_array_set(dev_a, 0);
    struct virtio_blk_dev *dev = byte_array_ptr(dev_a);

    dev->base = base;
    dev->lock = (spinlock_t) SPINLOCK_INITIALIZER;

    struct result res;

    /* Cache physical addresses for DMA-safe fields (never change). */
    struct result_paddr_t rp = virt_to_phys((vaddr_t) &dev->req);
    struct result_paddr_t sp = virt_to_phys((vaddr_t) &dev->status_byte);
    if (rp.is_error || sp.is_error) {
        res = result_error(EINVAL);
        goto err_free_dev;
    }
    dev->req_pa = result_paddr_t_checked(rp);
    dev->st_pa = result_paddr_t_checked(sp);

    /* Reset device, then set ACKNOWLEDGE + DRIVER. */
    vmio_reset(base);
    vmio_set_driver(base);

    /* Feature negotiation. */
    mmio_write32(base + VMIO_DEV_FEAT_SEL, 0);
    (void) mmio_read32(base + VMIO_DEV_FEAT);

    mmio_write32(base + VMIO_DRV_FEAT_SEL, 0);
    mmio_write32(base + VMIO_DRV_FEAT, 0); /* No special features needed. */

    if (version == VMIO_VERSION_MODERN) {
        mmio_write32(base + VMIO_DRV_FEAT_SEL, 1);
        mmio_write32(base + VMIO_DRV_FEAT, VIRTIO_F_VERSION_1);

        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE |
                                             VIRTIO_STATUS_DRIVER |
                                             VIRTIO_STATUS_FEATURES_OK);
        if (!(mmio_read32(base + VMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(ENODEV);
            goto err_free_dev;
        }
    } else {
        mmio_write32(base + VMIO_GUEST_PAGE_SZ, (u32) VMIO_PAGE_SIZE);
    }

    /* Read capacity from device config (u64 LE at offset 0). */
    u32 cap_lo = mmio_read32(base + VMIO_CONFIG);
    u32 cap_hi = mmio_read32(base + VMIO_CONFIG + 4);
    dev->capacity = ((u64) cap_hi << 32) | (u64) cap_lo;

    /* Set up virtqueue. */
    mmio_write32(base + VMIO_QUEUE_SEL, 0);
    u16 max_size = (u16) mmio_read32(base + VMIO_QUEUE_NUM_MAX);
    /* Need at least 3 descriptors for req+data+status chain. */
    if (max_size < 3) {
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
        res = result_error(ENODEV);
        goto err_free_dev;
    }
    dev->vq.size = (max_size < VBQ_SIZE) ? max_size : VBQ_SIZE;
    mmio_write32(base + VMIO_QUEUE_NUM, dev->vq.size);

    res = vblk_vq_alloc(dev);
    if (res.is_error) {
        mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
        goto err_free_dev;
    }

    if (version == VMIO_VERSION_MODERN) {
        struct result_paddr_t dp = virt_to_phys((vaddr_t) dev->vq.desc);
        struct result_paddr_t ap = virt_to_phys((vaddr_t) dev->vq.avail);
        struct result_paddr_t up = virt_to_phys((vaddr_t) dev->vq.used);
        if (dp.is_error || ap.is_error || up.is_error) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(EINVAL);
            goto err_free_dev;
        }
        u64 dpa = (u64) result_paddr_t_checked(dp);
        u64 apa = (u64) result_paddr_t_checked(ap);
        u64 upa = (u64) result_paddr_t_checked(up);
        vmio_write_queue_addr(base, VMIO_QUEUE_DESC_LO, dpa);
        vmio_write_queue_addr(base, VMIO_QUEUE_AVAIL_LO, apa);
        vmio_write_queue_addr(base, VMIO_QUEUE_USED_LO, upa);
        mmio_write32(base + VMIO_QUEUE_READY, 1);
    } else {
        mmio_write32(base + VMIO_QUEUE_ALIGN, (u32) VMIO_PAGE_SIZE);
        struct result_paddr_t pa = virt_to_phys((vaddr_t) dev->vq.desc);
        if (pa.is_error) {
            mmio_write32(base + VMIO_STATUS, VIRTIO_STATUS_FAILED);
            res = result_error(EINVAL);
            goto err_free_dev;
        }
        paddr_t paddr = result_paddr_t_checked(pa);
        mmio_write32(base + VMIO_QUEUE_PFN,
                     (u32) ((u64) paddr / VMIO_PAGE_SIZE));
    }

    /* DRIVER_OK. */
    u32 final_status =
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        (version == VMIO_VERSION_MODERN ? VIRTIO_STATUS_FEATURES_OK : 0) |
        VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(base + VMIO_STATUS, final_status);

    /* Register IRQ handler. */
    res = request_irq((int) irq, vblk_isr, dev, "virtio-blk");
    if (res.is_error)
        goto err_free_dev;

    /* Create blkdev. */
    struct option_byte_array bd_m =
        kvalloc_alloc(sizeof(struct blkdev), alignof(struct blkdev));
    if (bd_m.is_none) {
        free_irq((int) irq);
        res = result_error(ENOMEM);
        goto err_free_dev;
    }
    struct blkdev *bdev = byte_array_ptr(option_byte_array_checked(bd_m));
    bdev->capacity = dev->capacity;
    bdev->read_sectors = vblk_read_sectors;
    bdev->write_sectors = vblk_write_sectors;
    bdev->read_sectors_async = vblk_read_sectors_async;
    bdev->write_sectors_async = vblk_write_sectors_async;
    bdev->private_data = dev;

    global_blkdev = bdev;

    printk(KERN_INFO,
           STR("virtio-blk: %lu sectors (%lu KiB) v%u MMIO=0x%lx IRQ=%u\n"),
           dev->capacity, dev->capacity * BLKDEV_SECTOR_SIZE / 1024,
           (u64) version, base, (u64) irq);

    return result_ok();

err_free_dev:
    kvalloc_free(dev_a);
    return res;
}

struct result virtio_blk_probe(void)
{
    struct result res = vmio_scan_slots(vblk_probe_slot);
    if (res.is_error)
        printk(KERN_INFO, STR("virtio-blk: no device found\n"));
    return res;
}

#if CONFIG_SEMIHOSTING
#include __INC_TEST(virtio_blk)
#endif
