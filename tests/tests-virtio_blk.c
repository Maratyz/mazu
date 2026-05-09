/* SPDX-License-Identifier: MIT */
#include <mazu/blkdev.h>
#include <mazu/selftest.h>

static i32 selftest_virtio_blk_probe(void)
{
    /* Probe if not already done (selftest mode runs before init). */
    if (!global_blkdev)
        virtio_blk_probe();
    if (!global_blkdev)
        return 0; /* skip: no block device in this QEMU config */
    if (global_blkdev->capacity == 0)
        return 1;
    return 0;
}

DEFINE_SELFTEST(virtio_blk_probe, selftest_virtio_blk_probe);

static i32 selftest_virtio_blk_rw(void)
{
    if (!global_blkdev)
        return 0; /* skip: no block device available */

    /* Writes a known pattern to sector 1, reads back, verifies. */
    byte wbuf[BLKDEV_SECTOR_SIZE];
    byte rbuf[BLKDEV_SECTOR_SIZE];
    for (sz i = 0; i < BLKDEV_SECTOR_SIZE; i++)
        wbuf[i] = (byte) (i & 0xff);

    struct result wr = global_blkdev->write_sectors(global_blkdev, 1, 1, wbuf);
    if (wr.is_error)
        return 1;

    byte_array_set(byte_array_new(rbuf, BLKDEV_SECTOR_SIZE), 0);
    struct result rr = global_blkdev->read_sectors(global_blkdev, 1, 1, rbuf);
    if (rr.is_error)
        return 1;

    for (sz i = 0; i < BLKDEV_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i])
            return 1;
    }

    return 0;
}

DEFINE_SELFTEST(virtio_blk_rw, selftest_virtio_blk_rw);

/* Async I/O selftest: write via sync, read back via async callback. */
static int async_test_status;
static bool async_test_done;

static void async_test_cb(void *ctx __unused, int status)
{
    async_test_status = status;
    __atomic_store_n(&async_test_done, true, __ATOMIC_RELEASE);
}

static i32 selftest_virtio_blk_async(void)
{
    if (!global_blkdev || !global_blkdev->read_sectors_async)
        return 0; /* skip: no block device or async not supported */

    /* Writes a known pattern via the sync path. */
    byte wbuf[BLKDEV_SECTOR_SIZE];
    byte rbuf[BLKDEV_SECTOR_SIZE];
    for (sz i = 0; i < BLKDEV_SECTOR_SIZE; i++)
        wbuf[i] = (byte) ((i + 0x42) & 0xff);

    struct result wr = global_blkdev->write_sectors(global_blkdev, 2, 1, wbuf);
    if (wr.is_error)
        return 1;

    /* Reads back via the async path. */
    byte_array_set(byte_array_new(rbuf, BLKDEV_SECTOR_SIZE), 0);
    __atomic_store_n(&async_test_done, false, __ATOMIC_RELAXED);
    async_test_status = -1;

    struct result rr = global_blkdev->read_sectors_async(
        global_blkdev, 2, 1, rbuf, async_test_cb, NULL);
    if (rr.is_error)
        return 1;

    /* Waits for completion (timeout ~1 second). */
    u64 deadline = time_rdtime() + 10000000ULL;
    while (!__atomic_load_n(&async_test_done, __ATOMIC_ACQUIRE)) {
        if (time_rdtime() > deadline)
            return 1; /* timeout */
    }

    if (async_test_status != 0)
        return 1;

    /* Verify data. */
    for (sz i = 0; i < BLKDEV_SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i])
            return 1;
    }
    return 0;
}

DEFINE_SELFTEST(virtio_blk_async, selftest_virtio_blk_async);
