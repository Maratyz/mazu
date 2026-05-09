/* SPDX-License-Identifier: MIT */
/* Virtual filesystem - mount table, path routing, dispatch, ramfs adapter.
 *
 * Longest-prefix match over a small static mount table (VFS_MAX_MOUNTS
 * entries).  No heap allocation; the table lives in BSS.
 */

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/print.h>
#include <mazu/ramfs.h>
#include <mazu/string.h>
#include <mazu/vfs.h>

/* Mount table */

static struct vfs_mount mount_table[VFS_MAX_MOUNTS];
static sz vfs_route(struct str path, struct str *remainder);

static inline struct str vfs_mount_prefix(const struct vfs_mount *m)
{
    return str_new((char *) m->prefix, m->prefix_len);
}

static inline bool vfs_path_is_absolute(struct str path)
{
    return path.dat != NULL && path.len > 0 && path.dat[0] == '/';
}

static struct vfs_mount *vfs_mount_from_file(const struct vfs_file *f)
{
    if (!f || f->mount_idx >= VFS_MAX_MOUNTS)
        return NULL;
    return &mount_table[f->mount_idx];
}

static struct vfs_mount *vfs_route_mount(struct str path, struct str *remainder)
{
    sz idx = vfs_route(path, remainder);
    if (idx < 0)
        return NULL;
    return &mount_table[idx];
}

struct result vfs_mount(struct str path, struct vfs_ops ops, void *ctx)
{
    if (!vfs_path_is_absolute(path)) {
        pr_debug(STR("vfs_mount: invalid path (NULL, empty, or relative)\n"));
        return result_error(EINVAL);
    }

    if (path.len >= 64) {
        pr_debug(STR("vfs_mount: path too long (%zu >= 64)\n"), path.len);
        return result_error(ENAMETOOLONG);
    }

    for (sz i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active)
            continue;
        if (str_is_equal(vfs_mount_prefix(&mount_table[i]), path)) {
            pr_debug(STR("vfs_mount: duplicate mount point\n"));
            return result_error(EEXIST);
        }
    }

    for (sz i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active)
            continue;
        memcpy(mount_table[i].prefix, path.dat, path.len);
        mount_table[i].prefix_len = path.len;
        mount_table[i].ops = ops;
        mount_table[i].ctx = ctx;
        mount_table[i].active = true;
        return result_ok();
    }

    pr_debug(STR("vfs_mount: mount table full\n"));
    return result_error(ENOMEM);
}

struct result vfs_unmount(struct str path)
{
    if (!vfs_path_is_absolute(path)) {
        pr_debug(STR("VFS: unmount failed: invalid path\n"));
        return result_error(EINVAL);
    }

    for (sz i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active)
            continue;
        if (str_is_equal(vfs_mount_prefix(&mount_table[i]), path)) {
            mount_table[i].active = false;
            pr_info(STR("VFS: unmounted %.*s\n"), (int) path.len, path.dat);
            return result_ok();
        }
    }
    pr_debug(STR("VFS: unmount failed: path not found %.*s\n"), (int) path.len,
             path.dat);
    return result_error(ENOENT);
}

/* Path routing - longest-prefix match.
 *
 * Returns the mount index whose prefix is the longest match for 'path',
 * or -1 if no mount matches.  Sets 'remainder' to the path tail after
 * the matched prefix.
 */
static sz vfs_route(struct str path, struct str *remainder)
{
    if (!vfs_path_is_absolute(path)) {
        pr_debug(STR("vfs_route: invalid path\n"));
        return -1;
    }

    sz best_idx = -1;
    sz best_len = -1;

    for (sz i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active)
            continue;
        sz plen = mount_table[i].prefix_len;

        if (plen <= 0 || plen > 64) {
            pr_debug(STR("vfs_route: invalid prefix length %zd\n"), plen);
            continue;
        }

        /* Root mount "/" matches everything. */
        if (plen == 1 && mount_table[i].prefix[0] == '/') {
            if (plen > best_len) {
                best_len = plen;
                best_idx = i;
            }
            continue;
        }

        /* Non-root: path must start with prefix, and the next char must
         * be '/' or path must be exactly the prefix.
         */
        if (path.len >= plen &&
            str_has_prefix(path, vfs_mount_prefix(&mount_table[i]))) {
            /* Boundary check: "/foo" must not match "/foobar". */
            if (path.len == plen || path.dat[plen] == '/') {
                if (plen > best_len) {
                    best_len = plen;
                    best_idx = i;
                }
            }
        }
    }

    if (best_idx < 0) {
        pr_debug(STR("vfs_route: no mount found for path\n"));
        return -1;
    }

    /* Compute remainder: strip the prefix. For root mount, keep the
     * entire path. For other mounts, the remainder is everything after
     * the prefix. If nothing remains, use "/".
     */
    sz plen = mount_table[best_idx].prefix_len;

    if (plen == 1 && mount_table[best_idx].prefix[0] == '/') {
        *remainder = path;
    } else if (path.len == plen) {
        *remainder = STR("/");
    } else {
        *remainder = str_new(path.dat + plen, path.len - plen);
    }

    return best_idx;
}

/* Dispatch functions */

struct result_vfs_file vfs_open(struct str path)
{
    if (!vfs_path_is_absolute(path))
        return result_vfs_file_error(EINVAL);

    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m) {
        pr_debug(STR("VFS: open failed: no mount found for path '%.*s'\n"),
                 (int) path.len, path.dat);
        return result_vfs_file_error(ENOENT);
    }

    if (!m->ops.open) {
        pr_debug(STR("VFS: open failed: mount '%.*s' has no open operation\n"),
                 (int) path.len, path.dat);
        return result_vfs_file_error(ENOSYS);
    }

    struct result_vfs_file res = m->ops.open(m->ctx, rem);
    if (!res.is_error)
        res.unchecked_result_value.mount_idx = (u8) (m - mount_table);
    return res;
}

void vfs_close(struct vfs_file *f)
{
    if (!f)
        return;

    struct vfs_mount *m = vfs_mount_from_file(f);
    if (m && m->ops.close)
        m->ops.close(m->ctx, f);
}

struct result_sz vfs_read(struct vfs_file *f, struct byte_buf *buf, sz off)
{
    if (!f) {
        pr_debug(STR("VFS: read failed: NULL file pointer\n"));
        return result_sz_error(EINVAL);
    }

    if (!buf) {
        pr_debug(STR("VFS: read failed: NULL buffer pointer\n"));
        return result_sz_error(EINVAL);
    }

    struct vfs_mount *m = vfs_mount_from_file(f);
    if (!m) {
        pr_debug(STR("VFS: read failed: invalid mount index %d\n"),
                 f->mount_idx);
        return result_sz_error(EINVAL);
    }
    if (!m->ops.read)
        return result_sz_error(ENOSYS);
    return m->ops.read(m->ctx, f, buf, off);
}

struct result_sz vfs_write(struct vfs_file *f, struct byte_view data, sz off)
{
    if (!f) {
        pr_debug(STR("VFS: write failed: NULL file pointer\n"));
        return result_sz_error(EINVAL);
    }

    struct vfs_mount *m = vfs_mount_from_file(f);
    if (!m) {
        pr_debug(STR("VFS: write failed: invalid mount index %d\n"),
                 f->mount_idx);
        return result_sz_error(EINVAL);
    }
    if (!m->ops.write)
        return result_sz_error(ENOSYS);
    return m->ops.write(m->ctx, f, data, off);
}

struct result_vfs_stat vfs_stat(struct str path)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_vfs_stat_error(ENOENT);

    if (!m->ops.stat)
        return result_vfs_stat_error(ENOSYS);
    return m->ops.stat(m->ctx, rem);
}

struct result_vfs_dirent vfs_readdir(struct str path, sz index)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_vfs_dirent_error(ENOENT);

    if (!m->ops.readdir)
        return result_vfs_dirent_error(ENOSYS);
    return m->ops.readdir(m->ctx, rem, index);
}

struct result vfs_create(struct str path)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_error(ENOENT);

    if (!m->ops.create)
        return result_error(ENOSYS);
    return m->ops.create(m->ctx, rem);
}

struct result vfs_unlink(struct str path)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_error(ENOENT);

    if (!m->ops.unlink)
        return result_error(ENOSYS);
    return m->ops.unlink(m->ctx, rem);
}

struct result vfs_mkdir(struct str path)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_error(ENOENT);

    if (!m->ops.mkdir)
        return result_error(ENOSYS);
    return m->ops.mkdir(m->ctx, rem);
}

struct result vfs_rmdir(struct str path)
{
    struct str rem;
    struct vfs_mount *m = vfs_route_mount(path, &rem);
    if (!m)
        return result_error(ENOENT);

    if (!m->ops.rmdir)
        return result_error(ENOSYS);
    return m->ops.rmdir(m->ctx, rem);
}

/* Init */

void vfs_init(struct ram_fs *rfs)
{
    assert(rfs);

    /* Clear mount table. */
    for (sz i = 0; i < VFS_MAX_MOUNTS; i++)
        mount_table[i].active = false;

    struct result res = vfs_mount(STR("/"), ramfs_vfs_ops(), rfs);
    assert(!res.is_error);

    pr_info(STR("VFS: mounted ramfs at /\n"));
}

/* Self-tests */

#include __INC_TEST(vfs)
