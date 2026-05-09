/* SPDX-License-Identifier: MIT */
/* Self-tests for devfs synthetic filesystem. */

#include <kernel/fs/devfs.h>
#include <mazu/selftest.h>

/* Open a devfs path and assign to 'var'; return 'rc' on failure. */
#define DEVFS_OPEN_OR_FAIL(var, path, rc)                           \
    struct result_vfs_file var##_res = devfs_open(NULL, STR(path)); \
    if (var##_res.is_error)                                         \
        return (rc);                                                \
    struct vfs_file var = result_vfs_file_checked(var##_res)

/* Test: /dev/null reads return 0 bytes (EOF). */
static i32 selftest_devfs_null_read(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/null", 1);
    byte tmp[32];
    struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
    struct result_sz rr = devfs_read(NULL, &f, &buf, 0);
    if (rr.is_error)
        return 1;

    /* /dev/null always returns 0 bytes. */
    if (result_sz_checked(rr) != 0)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_null_read, selftest_devfs_null_read);

/* Test: /dev/null writes succeed and report full length. */
static i32 selftest_devfs_null_write(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/null", 1);
    struct byte_view data = byte_view_from_str(STR("test data"));
    struct result_sz wr = devfs_write(NULL, &f, data, 0);
    if (wr.is_error)
        return 1;

    if (result_sz_checked(wr) != data.len)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_null_write, selftest_devfs_null_write);

/* Test: /dev/zero fills buffer with zeros. */
static i32 selftest_devfs_zero_read(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/zero", 1);
    byte tmp[16];
    /* Poison the buffer to verify it gets zeroed. */
    for (sz i = 0; i < (sz) sizeof(tmp); i++)
        tmp[i] = 0xff;
    struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
    struct result_sz rr = devfs_read(NULL, &f, &buf, 0);
    if (rr.is_error)
        return 1;

    sz n = result_sz_checked(rr);
    if (n != sizeof(tmp))
        return 1;

    for (sz i = 0; i < n; i++) {
        if (tmp[i] != 0)
            return 1;
    }

    return 0;
}

DEFINE_SELFTEST(devfs_zero_read, selftest_devfs_zero_read);

/* Test: /dev/sysname returns "mazu\n". */
static i32 selftest_devfs_sysname(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/sysname", 1);
    byte tmp[32];
    struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
    struct result_sz rr = devfs_read(NULL, &f, &buf, 0);
    if (rr.is_error)
        return 1;

    struct str got = str_from_byte_buf(buf);
    if (!str_is_equal(got, STR("mazu\n")))
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_sysname, selftest_devfs_sysname);

/* Test: /dev/time returns a non-empty decimal string. */
static i32 selftest_devfs_time(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/time", 1);
    byte tmp[64];
    struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
    struct result_sz rr = devfs_read(NULL, &f, &buf, 0);
    if (rr.is_error)
        return 1;

    sz n = result_sz_checked(rr);
    if (n < 2) /* at least one digit + newline */
        return 1;

    /* Verify all characters before newline are digits. */
    for (sz i = 0; i < n - 1; i++) {
        if (tmp[i] < '0' || tmp[i] > '9')
            return 1;
    }
    if (tmp[n - 1] != '\n')
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_time, selftest_devfs_time);

/* Test: open nonexistent device returns ENOENT. */
static i32 selftest_devfs_enoent(void)
{
    struct result_vfs_file fres = devfs_open(NULL, STR("/bogus"));
    if (!fres.is_error)
        return 1;
    if (fres.code != ENOENT)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_enoent, selftest_devfs_enoent);

/* Test: readdir enumerates all device entries. */
static i32 selftest_devfs_readdir(void)
{
    /* There should be at least 6 entries (null, zero, console, time,
     * sysname, osversion). Enumerate until ENOENT.
     */
    sz count = 0;
    for (sz i = 0; i < 32; i++) {
        struct result_vfs_dirent d = devfs_readdir(NULL, STR("/"), i);
        if (d.is_error) {
            if (d.code == ENOENT)
                break;
            return 1; /* unexpected error */
        }
        count++;
    }

    if (count < 6)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_readdir, selftest_devfs_readdir);

/* Test: stat on root returns directory type. */
static i32 selftest_devfs_stat_root(void)
{
    struct result_vfs_stat sr = devfs_stat(NULL, STR("/"));
    if (sr.is_error)
        return 1;

    struct vfs_stat st = result_vfs_stat_checked(sr);
    if (st.type != VFS_TYPE_DIR)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_stat_root, selftest_devfs_stat_root);

/* Test: write to read-only device returns EROFS. */
static i32 selftest_devfs_zero_write_denied(void)
{
    DEVFS_OPEN_OR_FAIL(f, "/zero", 1);
    struct byte_view data = byte_view_from_str(STR("test"));
    struct result_sz wr = devfs_write(NULL, &f, data, 0);
    if (!wr.is_error)
        return 1;
    if (wr.code != EROFS)
        return 1;

    return 0;
}

DEFINE_SELFTEST(devfs_zero_write_denied, selftest_devfs_zero_write_denied);
