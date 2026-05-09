/* SPDX-License-Identifier: MIT */
/* Block buffer cache.
 *
 * Caches recently accessed sectors in RAM with LRU eviction.
 * Ordered flush (data before metadata) provides crash consistency
 * for on-disk filesystems.
 */

#ifndef MAZU_BCACHE_H
#define MAZU_BCACHE_H

#include <mazu/base.h>
#include <mazu/blkdev.h>
#include <mazu/error.h>
#include <mazu/list.h>
#include <mazu/spinlock.h>

#define BCACHE_SECTOR_SIZE 512

#define BUF_VALID BIT(0)
#define BUF_DIRTY BIT(1)
#define BUF_META BIT(2)

struct buf {
    u64 sector;
    u8 flags;
    u32 refcount;
    struct list_head lru_link;
    byte data[BCACHE_SECTOR_SIZE];
};

struct bcache {
    struct blkdev *dev;
    struct buf *bufs;
    sz n_bufs;
    struct list_head lru_head; /* MRU at front (head->next), LRU at tail
                                  (head->prev)
 */
    spinlock_t lock;
    u32 hits;
    u32 misses;
};

struct result bcache_init(struct bcache *bc, struct blkdev *dev, sz n_bufs);
struct buf *bcache_read(struct bcache *bc, u64 sector);
void bcache_mark_dirty(struct buf *b);
void bcache_mark_meta(struct buf *b);
void bcache_release(struct buf *b);
struct result bcache_sync(struct bcache *bc);

#endif /* MAZU_BCACHE_H */
