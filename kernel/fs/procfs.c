/* SPDX-License-Identifier: MIT */
/* Plan 9-style /proc synthetic filesystem.
 *
 * Exposes kernel status as virtual read-only files.  Content is generated
 * on each read into the caller's byte_buf - no heap allocation.
 *
 * Files:
 *   /proc/meminfo   page allocator statistics
 *   /proc/uptime    boot-relative wall-clock time
 *   /proc/cpuinfo   per-hart online status and counters
 */

#include "procfs.h"
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/error.h>
#include <mazu/fmt.h>
#include <mazu/kvalloc.h>
#include <mazu/pcpu.h>
#include <mazu/string.h>
#include <mazu/time.h>
#include <mazu/vfs.h>

/* Virtual file handlers */

typedef struct result_sz (*proc_read_fn_t)(struct byte_buf *buf, sz off);

struct proc_entry {
    struct str name;
    proc_read_fn_t read;
};

/* /proc/meminfo: page allocator free/total counts.
 * Format:
 *   total_pages <N>
 *   free_pages <N>
 */
static struct result_sz proc_meminfo_read(struct byte_buf *buf, sz off)
{
    char tmp[128];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    struct kvalloc_stats st = kvalloc_stats_get();
    fmt_append_kv_u64(&sb, STR("total_pages"), (u64) st.total_pages);
    fmt_append_kv_u64(&sb, STR("free_pages"), (u64) st.free_pages);

    return vfs_synth_read(buf, str_from_buf(sb), off);
}

/* /proc/uptime: time since boot.
 * Format:
 *   uptime_ms <N>
 */
static struct result_sz proc_uptime_read(struct byte_buf *buf, sz off)
{
    char tmp[64];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    u64 ms = time_current_ms().ms;
    fmt_append_kv_u64(&sb, STR("uptime_ms"), ms);

    return vfs_synth_read(buf, str_from_buf(sb), off);
}

/* /proc/cpuinfo: per-hart status summary.
 * Format:
 *   cpu <cpuid> hartid <hartid> online <0|1> timer <N> exti <N> ssi <N>
 */
static struct result_sz proc_cpuinfo_read(struct byte_buf *buf, sz off)
{
    char tmp[512];
    struct str_buf sb = str_buf_new(tmp, 0, sizeof(tmp));

    u32 n = nr_cpus_online;
    if (n == 0)
        n = 1;
    if (n > MAX_CPUS)
        n = MAX_CPUS;

    for (u32 i = 0; i < n; i++) {
        str_buf_append(&sb, STR("cpu "));
        fmt_append_u64((u64) i, &sb);
        str_buf_append(&sb, STR(" hartid "));
        fmt_append_u64((u64) pcpu_array[i].hartid, &sb);
        str_buf_append(&sb, STR(" online "));
        fmt_append_u64(pcpu_array[i].online ? 1 : 0, &sb);
        str_buf_append(&sb, STR(" timer "));
        fmt_append_u64(pcpu_array[i].nr_timer, &sb);
        str_buf_append(&sb, STR(" exti "));
        fmt_append_u64(pcpu_array[i].nr_exti, &sb);
        str_buf_append(&sb, STR(" ssi "));
        fmt_append_u64(pcpu_array[i].nr_ssi, &sb);
        str_buf_append_char(&sb, '\n');
    }

    return vfs_synth_read(buf, str_from_buf(sb), off);
}

/* Proc file table */

static struct proc_entry proc_table[] = {
    {STR_STATIC("meminfo"), proc_meminfo_read},
    {STR_STATIC("uptime"), proc_uptime_read},
    {STR_STATIC("cpuinfo"), proc_cpuinfo_read},
};

#define PROC_TABLE_SIZE (countof(proc_table))

static struct str proc_name_at(const void *entries, sz idx)
{
    return ((const struct proc_entry *) entries)[idx].name;
}

/* VFS operations */

static struct result_vfs_file procfs_open(void *ctx __unused, struct str path)
{
    return vfs_flat_named_open(path, PROC_TABLE_SIZE, proc_table,
                               PROC_TABLE_SIZE, proc_name_at);
}

static struct result_sz procfs_read(void *ctx __unused,
                                    struct vfs_file *f,
                                    struct byte_buf *buf,
                                    sz off)
{
    sz idx = (sz) (uptr) f->private_data;
    if (idx < 0 || idx >= PROC_TABLE_SIZE)
        return result_sz_error(EISDIR);

    return proc_table[idx].read(buf, off);
}

static struct result_vfs_stat procfs_stat(void *ctx __unused, struct str path)
{
    if (vfs_path_is_root(path))
        return result_vfs_stat_ok(vfs_rdonly_dir_stat());

    sz idx =
        vfs_named_table_lookup(path, proc_table, PROC_TABLE_SIZE, proc_name_at);
    if (idx < 0)
        return result_vfs_stat_error(ENOENT);

    return result_vfs_stat_ok(vfs_rdonly_file_stat());
}

static struct result_vfs_dirent procfs_readdir(void *ctx __unused,
                                               struct str dirpath __unused,
                                               sz index)
{
    return vfs_flat_named_readdir(proc_table, PROC_TABLE_SIZE, index,
                                  proc_name_at);
}

struct vfs_ops procfs_vfs_ops(void)
{
    return (struct vfs_ops) {
        .open = procfs_open,
        .close = vfs_noop_close,
        .read = procfs_read,
        .write = vfs_readonly_write,
        .stat = procfs_stat,
        .readdir = procfs_readdir,
        .create = vfs_readonly_mutation,
        .unlink = vfs_readonly_mutation,
        .mkdir = vfs_readonly_mutation,
        .rmdir = vfs_readonly_mutation,
    };
}
