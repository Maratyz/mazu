/* SPDX-License-Identifier: MIT */
/* Shared mock block device for filesystem selftests.
 *
 * Used by tests-bcache.c and tests-sfs.c.  Each consumer defines
 * MOCK_BLKDEV_SECTORS before including this header to set the disk size.
 */

#ifndef TESTS_MOCK_BLKDEV_H
#define TESTS_MOCK_BLKDEV_H

#include <kernel/fs/bcache.h>

#ifndef MOCK_BLKDEV_SECTORS
#define MOCK_BLKDEV_SECTORS 64
#endif

static byte mock_disk[MOCK_BLKDEV_SECTORS][BCACHE_SECTOR_SIZE];

static struct result mock_blkdev_read(struct blkdev *dev __unused,
                                      u64 sector,
                                      u32 count,
                                      byte *buf)
{
    if (count > MOCK_BLKDEV_SECTORS || sector > MOCK_BLKDEV_SECTORS - count)
        return result_error(EIO);
    for (u32 i = 0; i < count; i++)
        memcpy(buf + (sz) i * BCACHE_SECTOR_SIZE, mock_disk[sector + i],
               BCACHE_SECTOR_SIZE);
    return result_ok();
}

static struct result mock_blkdev_write(struct blkdev *dev __unused,
                                       u64 sector,
                                       u32 count,
                                       const byte *buf)
{
    if (count > MOCK_BLKDEV_SECTORS || sector > MOCK_BLKDEV_SECTORS - count)
        return result_error(EIO);
    for (u32 i = 0; i < count; i++)
        memcpy(mock_disk[sector + i], buf + (sz) i * BCACHE_SECTOR_SIZE,
               BCACHE_SECTOR_SIZE);
    return result_ok();
}

static struct blkdev mock_blkdev = {
    .capacity = MOCK_BLKDEV_SECTORS,
    .read_sectors = mock_blkdev_read,
    .write_sectors = mock_blkdev_write,
    .private_data = NULL,
};

static void mock_blkdev_reset(void)
{
    memset(mock_disk, 0, sizeof(mock_disk));
}

#endif /* TESTS_MOCK_BLKDEV_H */
