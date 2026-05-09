/* SPDX-License-Identifier: MIT */
#include <mazu/selftest.h>
#include <mazu/vfs.h>

/* Forward declarations: save/restore helpers defined after setup. */
static void vfs_test_save(struct vfs_mount *saved);
static void vfs_test_restore(struct vfs_mount *saved);

static i32 selftest_vfs_mount_routing(void)
{
    struct vfs_mount saved[VFS_MAX_MOUNTS];
    vfs_test_save(saved);

    /* Create dummy ops; only open is needed for routing test. */
    struct vfs_ops ops_root = {0};
    struct vfs_ops ops_sub = {0};

    struct result r = vfs_mount(STR("/"), ops_root, (void *) 0x1);
    assert(!r.is_error);

    r = vfs_mount(STR("/sub"), ops_sub, (void *) 0x2);
    assert(!r.is_error);

    /* Route /sub/file should match /sub mount. */
    struct str rem;
    sz idx = vfs_route(STR("/sub/file"), &rem);
    assert(idx >= 0);
    assert(mount_table[idx].ctx == (void *) 0x2);
    assert(str_is_equal(rem, STR("/file")));

    /* Route /other should match root mount. */
    idx = vfs_route(STR("/other"), &rem);
    assert(idx >= 0);
    assert(mount_table[idx].ctx == (void *) 0x1);
    assert(str_is_equal(rem, STR("/other")));

    /* Route /sub exactly should match /sub mount. */
    idx = vfs_route(STR("/sub"), &rem);
    assert(idx >= 0);
    assert(mount_table[idx].ctx == (void *) 0x2);
    assert(str_is_equal(rem, STR("/")));

    /* Duplicate mount fails. */
    r = vfs_mount(STR("/sub"), ops_sub, (void *) 0x3);
    assert(r.is_error && r.code == EEXIST);

    /* Unmount works. */
    r = vfs_unmount(STR("/sub"));
    assert(!r.is_error);

    /* After unmount, /sub/file routes to root. */
    idx = vfs_route(STR("/sub/file"), &rem);
    assert(idx >= 0);
    assert(mount_table[idx].ctx == (void *) 0x1);

    vfs_test_restore(saved);
    return 0;
}

DEFINE_SELFTEST(vfs_mount_routing, selftest_vfs_mount_routing);

/* Helper: create a self-contained ramfs with test data for VFS tests. */
#include <mazu/kvalloc.h>

static struct ram_fs *vfs_test_setup(void)
{
    struct alloc a = {
        .a_ptr = NULL,
        .alloc = kvalloc_alloc_wrapper,
        .free = kvalloc_free_wrapper,
    };
    struct ram_fs *rfs = ram_fs_new(a);
    if (!rfs)
        return NULL;

    /* Create /hello.txt with some content. */
    struct result_ram_fs_node fres =
        ram_fs_create_file(rfs->root, STR("/hello.txt"), true);
    if (!fres.is_error) {
        struct ram_fs_node *node = result_ram_fs_node_checked(fres);
        struct byte_view data = byte_view_from_str(STR("hello world"));
        ram_fs_write(node, data, 0);
    }

    /* Create /web/ directory with a file. */
    ram_fs_create_dir(rfs->root, STR("/web"), false);
    ram_fs_create_file(rfs->root, STR("/web/index.html"), true);

    return rfs;
}

static void vfs_test_save(struct vfs_mount *saved)
{
    for (sz i = 0; i < VFS_MAX_MOUNTS; i++) {
        saved[i] = mount_table[i];
        mount_table[i].active = false;
    }
}

static void vfs_test_restore(struct vfs_mount *saved)
{
    for (sz i = 0; i < VFS_MAX_MOUNTS; i++)
        mount_table[i] = saved[i];
}

/* Mount a test ramfs at "/", run body(rfs), restore mount table.
 * Returns 1 on setup failure, otherwise the body's return value.
 */
static i32 vfs_test_with_ramfs(i32 (*body)(struct ram_fs *))
{
    struct vfs_mount saved[VFS_MAX_MOUNTS];
    vfs_test_save(saved);

    struct ram_fs *rfs = vfs_test_setup();
    if (!rfs) {
        vfs_test_restore(saved);
        return 1;
    }

    struct result mr = vfs_mount(STR("/"), ramfs_vfs_ops(), rfs);
    if (mr.is_error) {
        vfs_test_restore(saved);
        return 1;
    }

    i32 ret = body(rfs);
    vfs_test_restore(saved);
    return ret;
}

static i32 vfs_open_read_body(struct ram_fs *rfs __unused)
{
    struct result_vfs_file fres = vfs_open(STR("/hello.txt"));
    if (fres.is_error)
        return 1;

    struct vfs_file f = result_vfs_file_checked(fres);
    byte tmp[64];
    struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
    struct result_sz rr = vfs_read(&f, &buf, 0);
    vfs_close(&f);

    if (rr.is_error)
        return 1;
    if (result_sz_checked(rr) == 0)
        return 1;
    return 0;
}

static i32 selftest_vfs_open_read(void)
{
    return vfs_test_with_ramfs(vfs_open_read_body);
}

DEFINE_SELFTEST(vfs_open_read, selftest_vfs_open_read);

static i32 vfs_readdir_body(struct ram_fs *rfs __unused)
{
    struct result_vfs_dirent d = vfs_readdir(STR("/web"), 0);
    if (d.is_error)
        return 1;
    struct vfs_dirent ent = result_vfs_dirent_checked(d);
    if (ent.name.len == 0)
        return 1;

    /* Past-end should return ENOENT. */
    d = vfs_readdir(STR("/web"), 10000);
    if (!d.is_error)
        return 1;
    if (d.code != ENOENT)
        return 1;

    return 0;
}

static i32 selftest_vfs_readdir(void)
{
    return vfs_test_with_ramfs(vfs_readdir_body);
}

DEFINE_SELFTEST(vfs_readdir, selftest_vfs_readdir);

static i32 selftest_vfs_unsupported_ops(void)
{
    struct vfs_mount saved[VFS_MAX_MOUNTS];
    vfs_test_save(saved);

    struct ram_fs *rfs = vfs_test_setup();
    if (!rfs) {
        vfs_test_restore(saved);
        return 1;
    }

    struct vfs_ops ops = ramfs_vfs_ops();
    ops.write = NULL;

    struct result r = vfs_mount(STR("/"), ops, rfs);
    if (r.is_error) {
        vfs_test_restore(saved);
        return 1;
    }

    struct result_vfs_file fres = vfs_open(STR("/hello.txt"));
    if (fres.is_error) {
        vfs_test_restore(saved);
        return 1;
    }

    struct vfs_file f = result_vfs_file_checked(fres);
    struct result_sz wr = vfs_write(&f, byte_view_from_str(STR("test")), 0);
    vfs_close(&f);
    vfs_test_restore(saved);

    if (!wr.is_error)
        return 1;
    if (wr.code != ENOSYS)
        return 1;

    return 0;
}

DEFINE_SELFTEST(vfs_unsupported_ops, selftest_vfs_unsupported_ops);
