/* SPDX-License-Identifier: MIT */
/* Block device abstraction.
 *
 * A blkdev represents a sector-addressable storage device.  All I/O
 * is in units of 512-byte sectors.
 */

#ifndef MAZU_BLKDEV_H
#define MAZU_BLKDEV_H

#include <mazu/base.h>
#include <mazu/error.h>

#define BLKDEV_SECTOR_SIZE 512

/* Async I/O completion callback.  status is 0 on success, negative errno
 * on failure.  Called from ISR context - must not sleep.
 */
typedef void (*blk_callback_t)(void *ctx, int status);

struct blkdev {
    u64 capacity; /* total sectors */
    struct result (*read_sectors)(struct blkdev *dev,
                                  u64 sector,
                                  u32 count,
                                  byte *buf);
    struct result (*write_sectors)(struct blkdev *dev,
                                   u64 sector,
                                   u32 count,
                                   const byte *buf);
    /* Async read: submit request and return immediately.  Completion
     * is signaled via the callback in ISR context.
     */
    struct result (*read_sectors_async)(struct blkdev *dev,
                                        u64 sector,
                                        u32 count,
                                        byte *buf,
                                        blk_callback_t cb,
                                        void *cb_ctx);
    struct result (*write_sectors_async)(struct blkdev *dev,
                                         u64 sector,
                                         u32 count,
                                         const byte *buf,
                                         blk_callback_t cb,
                                         void *cb_ctx);
    void *private_data;
};

/* Global block device (set by driver probe). NULL if no block device present.
 */
extern struct blkdev *global_blkdev;

#endif /* MAZU_BLKDEV_H */
