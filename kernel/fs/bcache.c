/* SPDX-License-Identifier: MIT */
/* Block buffer cache with LRU eviction and ordered flush.
 *
 * Each buf holds one 512-byte sector.  On access, the buf moves to
 * the MRU end of the LRU list.  Eviction picks the LRU tail with
 * refcount == 0.  Dirty bufs are written back before eviction.
 *
 * bcache_sync() flushes in two passes: data sectors first, then
 * metadata (superblock, bitmap, directory) - providing ordered writes
 * for crash consistency.
 */

#include "bcache.h"
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/kvalloc.h>
#include <mazu/print.h>
#include <mazu/string.h>

/* Internal helpers

 * Move buf to MRU position (head of LRU list).
 */
static void bcache_touch(struct bcache *bc, struct buf *b)
{
    list_del(&b->lru_link);
    list_add(&bc->lru_head, &b->lru_link);
}

/* Write a dirty buf back to disk and clear the dirty flag. */
static struct result bcache_flush_buf(struct bcache *bc, struct buf *b)
{
    if (!(b->flags & BUF_DIRTY))
        return result_ok();

    struct result res = bc->dev->write_sectors(bc->dev, b->sector, 1, b->data);
    if (!res.is_error)
        b->flags &= (u8) ~BUF_DIRTY;
    return res;
}

/* Find a buf for the given sector (already cached). */
static struct buf *bcache_lookup(struct bcache *bc, u64 sector)
{
    for (sz i = 0; i < bc->n_bufs; i++) {
        if ((bc->bufs[i].flags & BUF_VALID) && bc->bufs[i].sector == sector)
            return &bc->bufs[i];
    }
    return NULL;
}

/* Find an evictable buf (LRU, refcount == 0). Flushes dirty bufs before
 * eviction.
 */
static struct buf *bcache_evict(struct bcache *bc)
{
    /* Walk from LRU tail (head->prev) toward MRU (head->next). */
    struct list_head *pos = bc->lru_head.prev;
    while (pos != &bc->lru_head) {
        struct buf *b = __container_of(pos, struct buf, lru_link);
        if (b->refcount == 0) {
            if (b->flags & BUF_DIRTY) {
                struct result res = bcache_flush_buf(bc, b);
                if (res.is_error)
                    continue; /* skip this buf, try next eviction candidate */
            }
            return b;
        }
        pos = pos->prev;
    }
    return NULL;
}

/* Public API */

struct result bcache_init(struct bcache *bc, struct blkdev *dev, sz n_bufs)
{
    assert(bc && dev && n_bufs > 0);

    struct option_byte_array mem =
        kvalloc_alloc(n_bufs * sizeof(struct buf), alignof(struct buf));
    if (mem.is_none)
        return result_error(ENOMEM);

    struct byte_array ba = option_byte_array_checked(mem);
    byte_array_set(ba, 0);

    bc->dev = dev;
    bc->bufs = byte_array_ptr(ba);
    bc->n_bufs = n_bufs;
    bc->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    bc->hits = 0;
    bc->misses = 0;
    list_init(&bc->lru_head);

    /* Put all bufs on the LRU list (all initially invalid). */
    for (sz i = 0; i < n_bufs; i++) {
        bc->bufs[i].flags = 0;
        bc->bufs[i].refcount = 0;
        bc->bufs[i].sector = 0;
        list_init(&bc->bufs[i].lru_link);
        list_add_tail(&bc->lru_head, &bc->bufs[i].lru_link);
    }

    return result_ok();
}

struct buf *bcache_read(struct bcache *bc, u64 sector)
{
    assert(bc);

    u64 flags = spin_lock_irqsave(&bc->lock);

    /* Cache hit? */
    struct buf *b = bcache_lookup(bc, sector);
    if (b) {
        bc->hits++;
        b->refcount++;
        bcache_touch(bc, b);
        spin_unlock_irqrestore(&bc->lock, flags);
        return b;
    }

    /* Cache miss: evict and load. */
    bc->misses++;
    b = bcache_evict(bc);
    if (!b) {
        /* All bufs pinned; should not happen with reasonable sizing. */
        spin_unlock_irqrestore(&bc->lock, flags);
        return NULL;
    }

    b->sector = sector;
    b->flags = 0;
    b->refcount = 1;
    bcache_touch(bc, b);

    spin_unlock_irqrestore(&bc->lock, flags);

    /* Read from disk outside the lock. */
    struct result res = bc->dev->read_sectors(bc->dev, sector, 1, b->data);

    u64 re_flags = spin_lock_irqsave(&bc->lock);

    if (res.is_error) {
        /* Mark invalid and release. */
        b->flags = 0;
        b->refcount = 0;
        spin_unlock_irqrestore(&bc->lock, re_flags);
        return NULL;
    }

    b->flags = BUF_VALID;
    spin_unlock_irqrestore(&bc->lock, re_flags);
    return b;
}

void bcache_mark_dirty(struct buf *b)
{
    assert(b);
    b->flags |= BUF_DIRTY;
}

void bcache_mark_meta(struct buf *b)
{
    assert(b);
    b->flags |= BUF_META;
}

void bcache_release(struct buf *b)
{
    assert(b && b->refcount > 0);
    b->refcount--;
}

struct result bcache_sync(struct bcache *bc)
{
    assert(bc);

    /* Pass 1: flush data sectors (dirty but not meta). */
    for (sz i = 0; i < bc->n_bufs; i++) {
        struct buf *b = &bc->bufs[i];
        if ((b->flags & BUF_DIRTY) && !(b->flags & BUF_META)) {
            struct result res = bcache_flush_buf(bc, b);
            if (res.is_error)
                return res;
        }
    }

    /* Pass 2: flush metadata sectors (dirty and meta). */
    for (sz i = 0; i < bc->n_bufs; i++) {
        struct buf *b = &bc->bufs[i];
        if ((b->flags & BUF_DIRTY) && (b->flags & BUF_META)) {
            struct result res = bcache_flush_buf(bc, b);
            if (res.is_error)
                return res;
        }
    }

    return result_ok();
}

/* Self-tests */

#include __INC_TEST(bcache)
