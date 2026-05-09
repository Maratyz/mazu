/* SPDX-License-Identifier: MIT */
#define MOCK_BLKDEV_SECTORS 64
#include "tests-common.h"
#include "tests-mock-blkdev.h"

static i32 selftest_bcache_hit_miss(void)
{
    mock_blkdev_reset();

    /* Write a pattern to mock sector 5. */
    for (sz i = 0; i < BCACHE_SECTOR_SIZE; i++)
        mock_disk[5][i] = (byte) (i & 0xff);

    struct bcache bc;
    SELFTEST_ASSERT(!bcache_init(&bc, &mock_blkdev, 8).is_error, 1);

    /* First read = miss. */
    struct buf *b = bcache_read(&bc, 5);
    SELFTEST_ASSERT(b != NULL, 2);
    SELFTEST_ASSERT(bc.misses == 1 && bc.hits == 0, 3);

    for (sz i = 0; i < BCACHE_SECTOR_SIZE; i++)
        SELFTEST_ASSERT(b->data[i] == (byte) (i & 0xff), 4);
    bcache_release(b);

    /* Second read = hit. */
    b = bcache_read(&bc, 5);
    SELFTEST_ASSERT(b != NULL, 5);
    SELFTEST_ASSERT(bc.hits == 1, 6);
    bcache_release(b);

    return 0;
}
DEFINE_SELFTEST(bcache_hit_miss, selftest_bcache_hit_miss);

static i32 selftest_bcache_dirty_sync(void)
{
    mock_blkdev_reset();

    struct bcache bc;
    SELFTEST_ASSERT(!bcache_init(&bc, &mock_blkdev, 8).is_error, 1);

    /* Read sector 3, modify, mark dirty, sync. */
    struct buf *b = bcache_read(&bc, 3);
    SELFTEST_ASSERT(b != NULL, 2);
    for (sz i = 0; i < BCACHE_SECTOR_SIZE; i++)
        b->data[i] = 0xAB;
    bcache_mark_dirty(b);
    bcache_release(b);

    SELFTEST_ASSERT(!bcache_sync(&bc).is_error, 3);

    for (sz i = 0; i < BCACHE_SECTOR_SIZE; i++)
        SELFTEST_ASSERT(mock_disk[3][i] == 0xAB, 4);

    return 0;
}
DEFINE_SELFTEST(bcache_dirty_sync, selftest_bcache_dirty_sync);

static i32 selftest_bcache_lru_eviction(void)
{
    mock_blkdev_reset();

    /* Small cache of 4 bufs. */
    struct bcache bc;
    SELFTEST_ASSERT(!bcache_init(&bc, &mock_blkdev, 4).is_error, 1);

    /* Fill cache with sectors 0..3. */
    for (u64 s = 0; s < 4; s++) {
        mock_disk[s][0] = (byte) s;
        struct buf *b = bcache_read(&bc, s);
        SELFTEST_ASSERT(b != NULL, 2);
        bcache_release(b);
    }

    /* Access sector 1 to make it MRU. */
    struct buf *b1 = bcache_read(&bc, 1);
    SELFTEST_ASSERT(b1 != NULL, 3);
    bcache_release(b1);

    /* Read sector 10; should evict sector 0 (oldest LRU). */
    mock_disk[10][0] = 0x42;
    struct buf *b10 = bcache_read(&bc, 10);
    SELFTEST_ASSERT(b10 != NULL, 4);
    SELFTEST_ASSERT(b10->data[0] == 0x42, 5);
    bcache_release(b10);

    /* Sector 0 should be evicted; reading it is a miss. */
    u32 misses_before = bc.misses;
    struct buf *b0 = bcache_read(&bc, 0);
    SELFTEST_ASSERT(b0 != NULL, 6);
    SELFTEST_ASSERT(bc.misses == misses_before + 1, 7);
    bcache_release(b0);

    return 0;
}
DEFINE_SELFTEST(bcache_lru_eviction, selftest_bcache_lru_eviction);

static u32 flush_order_counter;
static u32 data_flush_order;
static u32 meta_flush_order;

static struct result order_mock_write(struct blkdev *dev __unused,
                                      u64 sector,
                                      u32 count,
                                      const byte *buf __unused)
{
    (void) count;
    flush_order_counter++;
    if (sector == 10)
        data_flush_order = flush_order_counter;
    else if (sector == 20)
        meta_flush_order = flush_order_counter;
    return result_ok();
}

static i32 selftest_bcache_ordered_flush(void)
{
    mock_blkdev_reset();
    flush_order_counter = 0;
    data_flush_order = 0;
    meta_flush_order = 0;

    struct blkdev order_dev = {
        .capacity = MOCK_BLKDEV_SECTORS,
        .read_sectors = mock_blkdev_read,
        .write_sectors = order_mock_write,
        .private_data = NULL,
    };

    struct bcache bc;
    SELFTEST_ASSERT(!bcache_init(&bc, &order_dev, 8).is_error, 1);

    /* Read and dirty sector 10 (data) and sector 20 (meta). */
    struct buf *bd = bcache_read(&bc, 10);
    SELFTEST_ASSERT(bd != NULL, 2);
    bcache_mark_dirty(bd);
    bcache_release(bd);

    struct buf *bm = bcache_read(&bc, 20);
    SELFTEST_ASSERT(bm != NULL, 3);
    bcache_mark_dirty(bm);
    bcache_mark_meta(bm);
    bcache_release(bm);

    SELFTEST_ASSERT(!bcache_sync(&bc).is_error, 4);

    /* Data must be flushed before metadata. */
    SELFTEST_ASSERT(data_flush_order != 0 && meta_flush_order != 0, 5);
    SELFTEST_ASSERT(data_flush_order < meta_flush_order, 6);

    return 0;
}
DEFINE_SELFTEST(bcache_ordered_flush, selftest_bcache_ordered_flush);
