/* SPDX-License-Identifier: MIT */
/* Plan 9-style /net synthetic filesystem.
 *
 * Exposes network state as read-only virtual files.  Content is
 * generated on each read - no heap allocation.
 *
 * Files:
 *   /net/arp        ARP table (one line per entry)
 *   /net/iface      network interface info
 *   /net/tcp/stats  TCP stack statistics
 *
 * The /net/tcp subdirectory appears as a directory in readdir.
 */

#include "netfs.h"
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/error.h>
#include <mazu/fmt.h>
#include <mazu/string.h>
#include <mazu/vfs.h>

#if CONFIG_NET_TCP
#include <mazu/net/arp.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/tcp.h>
#endif

/* Internal path IDs */

#define NETFS_ID_ROOT 0      /* /net (directory) */
#define NETFS_ID_ARP 1       /* /net/arp */
#define NETFS_ID_IFACE 2     /* /net/iface */
#define NETFS_ID_TCP_DIR 3   /* /net/tcp (directory) */
#define NETFS_ID_TCP_STATS 4 /* /net/tcp/stats */

/* Path resolution */

static sz netfs_resolve(struct str path)
{
    if (vfs_path_is_root(path))
        return NETFS_ID_ROOT;

    struct str p = vfs_path_trim_leading_slash(path);

    /* Strip trailing slash. */
    if (p.len > 0 && p.dat[p.len - 1] == '/')
        p.len--;

#if CONFIG_NET_TCP
    if (str_is_equal(p, STR("arp")))
        return NETFS_ID_ARP;
    if (str_is_equal(p, STR("iface")))
        return NETFS_ID_IFACE;
    if (str_is_equal(p, STR("tcp")))
        return NETFS_ID_TCP_DIR;
    if (str_is_equal(p, STR("tcp/stats")))
        return NETFS_ID_TCP_STATS;
#endif

    return -1;
}

/* Read handlers */

#if CONFIG_NET_TCP

/* Callback context for ARP iteration. */
struct arp_fmt_ctx {
    struct str_buf *sb;
    struct arena *arn;
};

static void arp_fmt_cb(struct arp_entry_info info, void *ctx_ptr)
{
    struct arp_fmt_ctx *ctx = ctx_ptr;
    /* Reset arena per entry so formatting temps don't accumulate. */
    byte *saved_beg = ctx->arn->beg;
    struct str ip_str = ipv4_addr_format(info.ip_addr, ctx->arn);
    struct str mac_str = mac_addr_format(info.mac_addr, ctx->arn);

    str_buf_append(ctx->sb, ip_str);
    str_buf_append_char(ctx->sb, ' ');
    str_buf_append(ctx->sb, mac_str);
    str_buf_append_char(ctx->sb, ' ');
    fmt_append_u64(info.age_ms, ctx->sb);
    str_buf_append(ctx->sb, STR("ms\n"));

    /* Reclaim arena temps; strings are already copied to str_buf. */
    ctx->arn->beg = saved_beg;
}

static struct result_sz netfs_arp_read(struct byte_buf *buf, sz off)
{
    char tmp[512];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    /* Arena for formatting IP/MAC addresses. */
    char arn_mem[256];
    struct arena arn = arena_new(byte_array_new(arn_mem, sizeof(arn_mem)));

    struct arp_fmt_ctx ctx = {.sb = &sb, .arn = &arn};
    arp_for_each(arp_fmt_cb, &ctx);

    struct str content = str_from_buf(sb);
    if (content.len == 0)
        content = STR("(empty)\n");

    return vfs_synth_read(buf, content, off);
}

static struct result_sz netfs_iface_read(struct byte_buf *buf, sz off)
{
    char tmp[256];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    char arn_mem[128];
    struct arena arn = arena_new(byte_array_new(arn_mem, sizeof(arn_mem)));

    /* Show the default interface info.  Look up the expected IP from SLIRP
     * or show a generic message.
     */
    struct netdev *dev = netdev_lookup_ip_addr(ipv4_addr_new(192, 168, 100, 2));

    if (dev) {
        struct netdev_info info;
        netdev_get_info(dev, &info);
        str_buf_append(&sb, STR("ip "));
        str_buf_append(&sb, ipv4_addr_format(info.ip_addr, &arn));
        str_buf_append_char(&sb, '\n');

        str_buf_append(&sb, STR("mac "));
        str_buf_append(&sb, mac_addr_format(info.mac_addr, &arn));
        str_buf_append_char(&sb, '\n');

        str_buf_append(&sb, STR("mtu "));
        fmt_append_u64((u64) info.mtu, &sb);
        str_buf_append_char(&sb, '\n');
        netdev_put(dev);
    } else {
        str_buf_append(&sb, STR("(no interface)\n"));
    }

    return vfs_synth_read(buf, str_from_buf(sb), off);
}

static struct result_sz netfs_tcp_stats_read(struct byte_buf *buf, sz off)
{
    char tmp[384];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    struct tcp_stats st = tcp_stats_get();
    fmt_append_kv_u64(&sb, STR("connections"), (u64) st.n_connections);
    fmt_append_kv_u64(&sb, STR("bytes_tx"), (u64) st.bytes_tx);
    fmt_append_kv_u64(&sb, STR("bytes_rx"), (u64) st.bytes_rx);
    fmt_append_kv_u64(&sb, STR("retransmits"), (u64) st.retransmits);
    fmt_append_kv_u64(&sb, STR("pool_exhaustion"), (u64) st.pool_exhaustion);
    fmt_append_kv_u64(&sb, STR("sbq_mem"), (u64) st.sbq_mem);
    fmt_append_kv_u64(&sb, STR("recv_mem"), (u64) st.recv_mem);

    return vfs_synth_read(buf, str_from_buf(sb), off);
}

#endif /* CONFIG_NET_TCP */

/* VFS operations */

static struct result_vfs_file netfs_open(void *ctx __unused, struct str path)
{
    sz id = netfs_resolve(path);
    if (id < 0)
        return result_vfs_file_error(ENOENT);
    return result_vfs_file_ok(vfs_file_id(id));
}

static struct result_sz netfs_read(void *ctx __unused,
                                   struct vfs_file *f,
                                   struct byte_buf *buf,
                                   sz off)
{
    sz id = (sz) (uptr) f->private_data;

#if !CONFIG_NET_TCP
    (void) buf;
    (void) off;
#endif

    switch (id) {
    case NETFS_ID_ROOT:
    case NETFS_ID_TCP_DIR:
        return result_sz_error(EISDIR);
#if CONFIG_NET_TCP
    case NETFS_ID_ARP:
        return netfs_arp_read(buf, off);
    case NETFS_ID_IFACE:
        return netfs_iface_read(buf, off);
    case NETFS_ID_TCP_STATS:
        return netfs_tcp_stats_read(buf, off);
#endif
    default:
        return result_sz_error(ENOENT);
    }
}

static struct result_vfs_stat netfs_stat(void *ctx __unused, struct str path)
{
    sz id = netfs_resolve(path);
    if (id < 0)
        return result_vfs_stat_error(ENOENT);

    u8 type = (id == NETFS_ID_ROOT || id == NETFS_ID_TCP_DIR) ? VFS_TYPE_DIR
                                                              : VFS_TYPE_FILE;

    return result_vfs_stat_ok(type == VFS_TYPE_DIR ? vfs_rdonly_dir_stat()
                                                   : vfs_rdonly_file_stat());
}

#if CONFIG_NET_TCP
/* Root directory entries: arp, iface, tcp */
static struct str netfs_root_entries[] = {
    STR_STATIC("arp"),
    STR_STATIC("iface"),
    STR_STATIC("tcp"),
};

/* /net/tcp directory entries: stats */
static struct str netfs_tcp_entries[] = {
    STR_STATIC("stats"),
};
#endif

static struct result_vfs_dirent netfs_readdir(void *ctx __unused,
                                              struct str dirpath,
                                              sz index)
{
    sz id = netfs_resolve(dirpath);

#if CONFIG_NET_TCP
    if (id == NETFS_ID_ROOT) {
        if (index < 0 || index >= (sz) countof(netfs_root_entries))
            return result_vfs_dirent_error(ENOENT);

        u8 type = str_is_equal(netfs_root_entries[index], STR("tcp"))
                      ? VFS_TYPE_DIR
                      : VFS_TYPE_FILE;
        return result_vfs_dirent_ok((struct vfs_dirent) {
            .name = netfs_root_entries[index],
            .type = type,
        });
    }

    if (id == NETFS_ID_TCP_DIR) {
        if (index < 0 || index >= (sz) countof(netfs_tcp_entries))
            return result_vfs_dirent_error(ENOENT);

        return result_vfs_dirent_ok((struct vfs_dirent) {
            .name = netfs_tcp_entries[index],
            .type = VFS_TYPE_FILE,
        });
    }
#else
    (void) index;
#endif

    if (id == NETFS_ID_ROOT)
        return result_vfs_dirent_error(ENOENT);

    return result_vfs_dirent_error(ENOTDIR);
}

struct vfs_ops netfs_vfs_ops(void)
{
    return (struct vfs_ops) {
        .open = netfs_open,
        .close = vfs_noop_close,
        .read = netfs_read,
        .write = vfs_readonly_write,
        .stat = netfs_stat,
        .readdir = netfs_readdir,
        .create = vfs_readonly_mutation,
        .unlink = vfs_readonly_mutation,
        .mkdir = vfs_readonly_mutation,
        .rmdir = vfs_readonly_mutation,
    };
}
