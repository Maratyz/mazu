/* SPDX-License-Identifier: MIT */
#define MOCK_BLKDEV_SECTORS 2048
#include <kernel/fs/sfs.h>
#include "tests-common.h"
#include "tests-mock-blkdev.h"

/* Common setup: reset mock, init bcache, format, mount.
 * Declares: struct bcache bc, struct sfs *fs, struct vfs_ops ops.
 * Returns 1 on any init failure.
 */
#define SFS_TEST_SETUP()                               \
    mock_blkdev_reset();                               \
    struct bcache bc;                                  \
    if (bcache_init(&bc, &mock_blkdev, 32).is_error)   \
        return 1;                                      \
    if (sfs_format(&bc, MOCK_BLKDEV_SECTORS).is_error) \
        return 1;                                      \
    struct result_sfs mnt = sfs_mount(&bc);            \
    if (mnt.is_error)                                  \
        return 1;                                      \
    struct sfs *fs = result_sfs_checked(mnt);          \
    struct vfs_ops ops = sfs_vfs_ops();                \
    (void) ops

static i32 selftest_sfs_format_mount(void)
{
    SFS_TEST_SETUP();
    SELFTEST_ASSERT(fs->super.magic == SFS_MAGIC, 1);
    SELFTEST_ASSERT(fs->super.total_sectors == MOCK_BLKDEV_SECTORS, 2);
    return 0;
}
DEFINE_SELFTEST(sfs_format_mount, selftest_sfs_format_mount);

static i32 selftest_sfs_create_write_read(void)
{
    SFS_TEST_SETUP();

    SELFTEST_ASSERT(!ops.create(fs, STR("hello.txt")).is_error, 1);

    struct result_vfs_file of = ops.open(fs, STR("hello.txt"));
    SELFTEST_ASSERT(!of.is_error, 2);
    struct vfs_file f = result_vfs_file_checked(of);

    struct str content = STR("Hello SFS!");
    struct result_sz wr = ops.write(fs, &f, byte_view_from_str(content), 0);
    SELFTEST_ASSERT(!wr.is_error, 3);
    SELFTEST_ASSERT(result_sz_checked(wr) == content.len, 4);
    ops.close(fs, &f);

    /* Re-open and read back. */
    of = ops.open(fs, STR("hello.txt"));
    SELFTEST_ASSERT(!of.is_error, 5);
    f = result_vfs_file_checked(of);

    byte rbuf[64];
    struct byte_buf bb = byte_buf_new(rbuf, 0, sizeof(rbuf));
    struct result_sz rr = ops.read(fs, &f, &bb, 0);
    SELFTEST_ASSERT(!rr.is_error, 6);
    SELFTEST_ASSERT(result_sz_checked(rr) == content.len, 7);

    struct str got = str_new((char *) rbuf, bb.len);
    SELFTEST_ASSERT(str_is_equal(got, content), 8);
    ops.close(fs, &f);

    return 0;
}
DEFINE_SELFTEST(sfs_create_write_read, selftest_sfs_create_write_read);

static i32 selftest_sfs_unlink(void)
{
    SFS_TEST_SETUP();

    SELFTEST_ASSERT(!ops.create(fs, STR("temp.txt")).is_error, 1);
    SELFTEST_ASSERT(!ops.unlink(fs, STR("temp.txt")).is_error, 2);

    /* Should no longer be openable. */
    struct result_vfs_file of = ops.open(fs, STR("temp.txt"));
    SELFTEST_ASSERT(of.is_error, 3);

    return 0;
}
DEFINE_SELFTEST(sfs_unlink, selftest_sfs_unlink);

static i32 selftest_sfs_mkdir_rmdir(void)
{
    SFS_TEST_SETUP();

    SELFTEST_ASSERT(!ops.mkdir(fs, STR("subdir")).is_error, 1);

    struct result_vfs_stat st = ops.stat(fs, STR("subdir"));
    SELFTEST_ASSERT(!st.is_error, 2);
    SELFTEST_ASSERT(result_vfs_stat_checked(st).type == VFS_TYPE_DIR, 3);

    SELFTEST_ASSERT(!ops.rmdir(fs, STR("subdir")).is_error, 4);

    st = ops.stat(fs, STR("subdir"));
    SELFTEST_ASSERT(st.is_error, 5);

    return 0;
}
DEFINE_SELFTEST(sfs_mkdir_rmdir, selftest_sfs_mkdir_rmdir);

static i32 selftest_sfs_fsck(void)
{
    SFS_TEST_SETUP();

    SELFTEST_ASSERT(!ops.create(fs, STR("check.txt")).is_error, 1);
    struct result_vfs_file of = ops.open(fs, STR("check.txt"));
    SELFTEST_ASSERT(!of.is_error, 2);
    struct vfs_file f = result_vfs_file_checked(of);
    struct str data = STR("fsck test data");
    ops.write(fs, &f, byte_view_from_str(data), 0);
    ops.close(fs, &f);

    SELFTEST_ASSERT(!sfs_fsck(fs).is_error, 3);

    return 0;
}
DEFINE_SELFTEST(sfs_fsck, selftest_sfs_fsck);
