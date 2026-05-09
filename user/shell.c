/* SPDX-License-Identifier: MIT */
#include "shell.h"

#include <mazu/arena.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/eventlog.h>
#include <mazu/fmt.h>
#include <mazu/kvalloc.h>
#include <mazu/net/icmp.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/tcp.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/time.h>
#include <mazu/vfs.h>
#if CONFIG_NET_UDP
#include <mazu/net/dns.h>
#endif
#if CONFIG_VIRTIO_BLK
#include <mazu/blkdev.h>
#endif

#if CONFIG_NET_UDP
/* Resolve a hostname via the QEMU SLIRP DNS forwarder (192.168.100.3).
 * Allocates temporary buffers from kvalloc, frees them before returning.
 */
static struct result_ipv4_addr shell_dns_resolve(struct str hostname)
{
    struct option_byte_array sb_opt = kvalloc_alloc(0x2000, 64);
    struct option_byte_array dns_opt = kvalloc_alloc(0x1000, 64);
    if (sb_opt.is_none || dns_opt.is_none) {
        if (!sb_opt.is_none)
            kvalloc_free(sb_opt.unchecked_option_value);
        if (!dns_opt.is_none)
            kvalloc_free(dns_opt.unchecked_option_value);
        return result_ipv4_addr_error(ENOMEM);
    }
    struct byte_array sb_ba = option_byte_array_checked(sb_opt);
    struct send_buf sb = send_buf_new(arena_new(sb_ba));
    struct byte_array dns_ba = option_byte_array_checked(dns_opt);
    struct arena dns_arn = arena_new(dns_ba);

    struct result_ipv4_addr res =
        dns_resolve(ipv4_addr_new(192, 168, 100, 3), hostname, sb, dns_arn);
    kvalloc_free(dns_ba);
    kvalloc_free(sb_ba);
    return res;
}
#endif

static struct shell_session global_sessions[SHELL_MAX_SESSIONS];
/* Round-robin victim when all slots are full. */
static u32 global_evict_idx;

void shell_init(void)
{
    for (sz i = 0; i < SHELL_MAX_SESSIONS; i++)
        global_sessions[i].active = false;
}

static struct shell_session *shell_alloc_slot(u32 sid)
{
    struct shell_session *slot = NULL;

    for (sz i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (!global_sessions[i].active) {
            slot = &global_sessions[i];
            break;
        }
    }

    if (!slot) {
        /* All slots occupied; evict round-robin. */
        slot = &global_sessions[global_evict_idx % SHELL_MAX_SESSIONS];
        global_evict_idx++;
    }

    slot->active = true;
    slot->sid = sid;
    slot->write_total = 0;
    pseudo_rand_u64(&slot->token);
    return slot;
}

struct shell_session *shell_create(u32 sid)
{
    /* Deactivate any stale session with the same sid so that shell_find()
     * cannot return an old slot whose token no longer matches the client's.
     */
    for (sz i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (global_sessions[i].active && global_sessions[i].sid == sid)
            global_sessions[i].active = false;
    }
    return shell_alloc_slot(sid);
}

struct shell_session *shell_find_or_create(u32 sid)
{
    for (sz i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (global_sessions[i].active && global_sessions[i].sid == sid)
            return &global_sessions[i];
    }
    return shell_alloc_slot(sid);
}

struct shell_session *shell_find(u32 sid)
{
    for (sz i = 0; i < SHELL_MAX_SESSIONS; i++) {
        if (global_sessions[i].active && global_sessions[i].sid == sid)
            return &global_sessions[i];
    }
    return NULL;
}

/* Output helpers */

static void shell_write(struct shell_session *sess, struct str s)
{
    assert(sess);
    for (sz i = 0; i < s.len; i++) {
        sess->out[sess->write_total % SHELL_OUT_BUF_SIZE] = s.dat[i];
        sess->write_total++;
    }
}

/* Write a formatted string to the session output.  Uses a fixed 256-byte
 * stack buffer so it does not consume arena memory.
 */
#define SHELL_FMT_BUF_SIZE 256
static void shell_writef(struct shell_session *sess, struct str fmt_str, ...)
{
    char data[SHELL_FMT_BUF_SIZE];
    struct str_buf buf = str_buf_new(data, 0, SHELL_FMT_BUF_SIZE);
    va_list args;
    va_start(args, fmt_str);
    fmt_vfmt(&buf, fmt_str, args);
    va_end(args);
    shell_write(sess, str_from_buf(buf));
}

/* Command tokeniser */

static struct str shell_next_token(struct str *s)
{
    assert(s);
    while (s->len > 0 && (s->dat[0] == ' ' || s->dat[0] == '\t' ||
                          s->dat[0] == '\r' || s->dat[0] == '\n')) {
        s->dat++;
        s->len--;
    }
    if (s->len == 0)
        return str_new(NULL, 0);

    sz i = 0;
    while (i < s->len && s->dat[i] != ' ' && s->dat[i] != '\t' &&
           s->dat[i] != '\r' && s->dat[i] != '\n')
        i++;

    struct str token = str_new(s->dat, i);
    s->dat += i;
    s->len -= i;
    return token;
}

/* Commands */

static void cmd_help(struct shell_session *sess)
{
    shell_write(sess, STR("commands:\n"));
    shell_write(sess, STR("  help                  list commands\n"));
    shell_write(sess, STR("  ls <path>             list directory\n"));
    shell_write(sess, STR("  cat <path>            print file contents\n"));
    shell_write(sess, STR("  netstat               TCP connection table\n"));
    shell_write(sess, STR("  mem                   memory allocator stats\n"));
    shell_write(sess, STR("  tcpstats              aggregate TCP counters\n"));
#if CONFIG_VIRTIO_BLK
    shell_write(sess, STR("  blkinfo               block device info\n"));
#endif
    shell_write(sess, STR("  spawn <path>          load and exec user "
                          "binary\n"));
    shell_write(sess, STR("  ps                    kernel tasks and user "
                          "processes\n"));
    shell_write(sess, STR("  kill <pid>            terminate process\n"));
    shell_write(sess, STR("  echo <args>           print arguments\n"));
    shell_write(sess, STR("  uptime                time since boot\n"));
    shell_write(sess, STR("  ping <host>           send ICMP echo\n"));
#if CONFIG_NET_UDP
    shell_write(sess, STR("  nslookup <hostname>   DNS lookup\n"));
#endif
#if CONFIG_RAMFS_WRITABLE
    shell_write(sess, STR("  write <path> <text>   create/write file\n"));
    shell_write(sess, STR("  rm <path>             delete file\n"));
    shell_write(sess, STR("  mkdir <path>          create directory\n"));
#endif
}

static void cmd_ls(struct shell_session *sess, struct str path)
{
    if (path.len == 0)
        path = STR("/");

    struct result_vfs_stat st_res = vfs_stat(path);
    if (st_res.is_error) {
        shell_writef(sess, STR("ls: \"%s\": not found\n"), path);
        return;
    }
    struct vfs_stat st = result_vfs_stat_checked(st_res);
    if (st.type == VFS_TYPE_FILE) {
        /* Print just the filename. */
        shell_writef(sess, STR("%s\n"), path);
        return;
    }
    for (sz idx = 0;; idx++) {
        struct result_vfs_dirent d = vfs_readdir(path, idx);
        if (d.is_error)
            break;
        struct vfs_dirent ent = result_vfs_dirent_checked(d);
        if (ent.type == VFS_TYPE_DIR)
            shell_writef(sess, STR("%s/\n"), ent.name);
        else
            shell_writef(sess, STR("%s\n"), ent.name);
    }
}

static void cmd_cat(struct shell_session *sess, struct str path)
{
    if (path.len == 0) {
        shell_write(sess, STR("cat: missing path\n"));
        return;
    }

    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error) {
        shell_writef(sess, STR("cat: \"%s\": not found\n"), path);
        return;
    }
    struct vfs_file f = result_vfs_file_checked(fres);

    /* Check if it's a directory via stat. */
    struct result_vfs_stat st_res = vfs_stat(path);
    if (!st_res.is_error) {
        struct vfs_stat st = result_vfs_stat_checked(st_res);
        if (st.type == VFS_TYPE_DIR) {
            shell_writef(sess, STR("cat: \"%s\": is a directory\n"), path);
            vfs_close(&f);
            return;
        }
    }

    /* Read the file content in chunks. */
    byte tmp[4096];
    sz off = 0;
    for (;;) {
        struct byte_buf buf = byte_buf_new(tmp, 0, sizeof(tmp));
        struct result_sz rr = vfs_read(&f, &buf, off);
        if (rr.is_error) {
            shell_writef(sess, STR("cat: \"%s\": read failed\n"), path);
            break;
        }

        sz n_read = result_sz_checked(rr);
        if (n_read == 0)
            break;

        shell_write(sess, str_new((char *) tmp, buf.len));

        if (n_read < sizeof(tmp))
            break;

        if (off > SZ_MAX - n_read)
            break;
        off += n_read;
    }

    vfs_close(&f);
}

struct netstat_ctx {
    struct shell_session *sess;
    sz n;
};

static void netstat_cb(struct tcp_conn_info info, void *ctx_ptr)
{
    struct netstat_ctx *ctx = ctx_ptr;
    char host_buf[IP_ADDR_FMT_BUF_SIZE];
    char peer_buf[IP_ADDR_FMT_BUF_SIZE];
    struct arena host_arn =
        arena_new(byte_array_new(host_buf, sizeof(host_buf)));
    struct arena peer_arn =
        arena_new(byte_array_new(peer_buf, sizeof(peer_buf)));
    struct str host = ipv4_addr_format(info.host_addr, &host_arn);
    struct str peer = ipv4_addr_format(info.peer_addr, &peer_arn);

    shell_writef(ctx->sess, STR("%s %s:%u %s:%u snd=%lu rcv=%lu\n"),
                 info.state_name, host, (u32) info.host_port, peer,
                 (u32) info.peer_port, (u64) info.send_next,
                 (u64) info.recv_next);
    ctx->n++;
}

static void cmd_netstat(struct shell_session *sess)
{
    shell_write(sess, STR("State         Local              Remote             "
                          "SND.NXT    RCV.NXT\n"));
    struct netstat_ctx ctx = {.sess = sess, .n = 0};
    tcp_for_each_conn(netstat_cb, &ctx);
    if (ctx.n == 0)
        shell_write(sess, STR("(no TCP connections)\n"));
}

static void cmd_mem(struct shell_session *sess)
{
    struct kvalloc_stats ks = kvalloc_stats_get();
    shell_writef(sess,
                 STR("kvalloc: total=%ld pages  free=%ld pages  used=%ld "
                     "pages\n"),
                 ks.total_pages, ks.free_pages, ks.total_pages - ks.free_pages);
}

struct tasks_ctx {
    struct shell_session *sess;
    sz n;
};

static void tasks_cb(struct sched_task_info info, void *ctx_ptr)
{
    struct tasks_ctx *ctx = ctx_ptr;
    shell_writef(ctx->sess, STR("  %-4u  %-8s  %10lu us  0x%lx\n"),
                 (u32) info.id, td_state_name(info.state), info.cpu_time_us,
                 (u64) (uptr) info.callback);
    ctx->n++;
}

#if CONFIG_RAMFS_WRITABLE
static void cmd_write(struct shell_session *sess,
                      struct str path,
                      struct str content)
{
    if (path.len == 0) {
        shell_write(sess, STR("write: missing path\n"));
        return;
    }
    if (content.len == 0) {
        shell_write(sess, STR("write: missing content\n"));
        return;
    }

    /* Ensure the file exists (create if needed). */
    struct result cr = vfs_create(path);
    if (cr.is_error && cr.code != EEXIST) {
        shell_writef(sess, STR("write: create \"%s\" failed: %hu\n"), path,
                     cr.code);
        return;
    }

    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error) {
        shell_writef(sess, STR("write: open \"%s\" failed\n"), path);
        return;
    }
    struct vfs_file f = result_vfs_file_checked(fres);
    struct result_sz wr = vfs_write(&f, byte_view_from_str(content), 0);
    vfs_close(&f);

    if (wr.is_error)
        shell_writef(sess, STR("write: error %hu\n"), wr.code);
    else
        shell_writef(sess, STR("wrote %ld bytes\n"), result_sz_checked(wr));
}

static void cmd_rm(struct shell_session *sess, struct str path)
{
    if (path.len == 0) {
        shell_write(sess, STR("rm: missing path\n"));
        return;
    }
    struct result r = vfs_unlink(path);
    if (r.is_error)
        shell_writef(sess, STR("rm: \"%s\": error %hu\n"), path, r.code);
}

static void cmd_mkdir(struct shell_session *sess, struct str path)
{
    if (path.len == 0) {
        shell_write(sess, STR("mkdir: missing path\n"));
        return;
    }
    struct result r = vfs_mkdir(path);
    if (r.is_error)
        shell_writef(sess, STR("mkdir: \"%s\": error %hu\n"), path, r.code);
}
#endif /* CONFIG_RAMFS_WRITABLE */

static void cmd_tcpstats(struct shell_session *sess)
{
    struct tcp_stats s = tcp_stats_get();
    shell_writef(sess, STR("uptime:      %lu ms\n"), s.uptime.ms);
    shell_writef(sess, STR("connections: %ld\n"), s.n_connections);
    shell_writef(sess, STR("sbq-mem:     %ld bytes\n"), s.sbq_mem);
    shell_writef(sess, STR("recv-mem:    %ld bytes\n"), s.recv_mem);
    shell_writef(sess, STR("tx-bytes:    %ld\n"), s.bytes_tx);
    shell_writef(sess, STR("rx-bytes:    %ld\n"), s.bytes_rx);
}

#if CONFIG_VIRTIO_BLK
static void cmd_blkinfo(struct shell_session *sess)
{
    if (!global_blkdev) {
        shell_write(sess, STR("blkinfo: no block device\n"));
        return;
    }
    shell_writef(sess, STR("capacity: %lu sectors (%lu KiB)\n"),
                 global_blkdev->capacity,
                 global_blkdev->capacity * BLKDEV_SECTOR_SIZE / 1024);
}
#endif

/* Network commands */

static void cmd_ping(struct shell_session *sess,
                     struct str target,
                     struct arena tmp)
{
    if (target.len == 0) {
        shell_write(sess, STR("ping: missing host\n"));
        return;
    }

    /* Parse as IP address first. */
    struct result_ipv4_addr_parsed parsed = ipv4_addr_parse(target);
    struct ipv4_addr dest;

    if (!parsed.is_error) {
        dest = result_ipv4_addr_parsed_checked(parsed).addr;
    } else {
#if CONFIG_NET_UDP
        struct result_ipv4_addr dns_res = shell_dns_resolve(target);
        if (dns_res.is_error) {
            shell_writef(sess, STR("ping: cannot resolve \"%s\"\n"), target);
            return;
        }
        dest = result_ipv4_addr_checked(dns_res);
#else
        shell_writef(sess, STR("ping: invalid IP \"%s\"\n"), target);
        return;
#endif
    }

    shell_writef(sess, STR("PING %s\n"), ipv4_addr_format(dest, &tmp));

    struct ping_stats stats =
        icmp_ping(dest, (u16) (time_rdtime() & 0xFFFF), 4);

    shell_writef(sess, STR("--- %s ping statistics ---\n"),
                 ipv4_addr_format(dest, &tmp));
    shell_writef(sess, STR("%ld packets transmitted, %ld received\n"),
                 (i64) stats.sent, (i64) stats.received);
    if (stats.received > 0)
        shell_writef(sess, STR("rtt min/avg/max = %lu/%lu/%lu us\n"),
                     stats.min_rtt_us, stats.avg_rtt_us, stats.max_rtt_us);
}

#if CONFIG_NET_UDP
static void cmd_nslookup(struct shell_session *sess,
                         struct str hostname,
                         struct arena tmp)
{
    if (hostname.len == 0) {
        shell_write(sess, STR("nslookup: missing hostname\n"));
        return;
    }

    struct result_ipv4_addr res = shell_dns_resolve(hostname);
    if (res.is_error) {
        shell_writef(sess, STR("nslookup: \"%s\" failed (error %hu)\n"),
                     hostname, res.code);
        return;
    }

    struct ipv4_addr ns = ipv4_addr_new(192, 168, 100, 3);
    struct ipv4_addr addr = result_ipv4_addr_checked(res);
    shell_writef(sess, STR("Server:  %s\n"), ipv4_addr_format(ns, &tmp));
    shell_writef(sess, STR("Name:    %s\n"), hostname);
    shell_writef(sess, STR("Address: %s\n"), ipv4_addr_format(addr, &tmp));
}
#endif /* CONFIG_NET_UDP */

/* Process commands */

static void cmd_spawn(struct shell_session *sess, struct str path)
{
    if (path.len <= 0) {
        shell_write(sess, STR("spawn: missing path\n"));
        return;
    }

    /* Read the binary from VFS. */
    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error) {
        shell_writef(sess, STR("spawn: \"%s\": not found\n"), path);
        return;
    }
    struct vfs_file f = result_vfs_file_checked(fres);

    byte buf[8192];
    struct byte_buf bb = byte_buf_new(buf, 0, sizeof(buf));
    struct result_sz rr = vfs_read(&f, &bb, 0);
    vfs_close(&f);
    if (rr.is_error || bb.len == 0) {
        shell_writef(sess, STR("spawn: read \"%s\" failed\n"), path);
        return;
    }

    struct proc *p = proc_alloc();
    if (!p) {
        shell_write(sess, STR("spawn: process table full\n"));
        return;
    }

    /* Truncate to the fixed process name buffer and keep room for '\0'. */
    memset(p->name, 0, sizeof(p->name));
    sz name_cap = countof(p->name) - 1;
    sz namelen = path.len < name_cap ? path.len : name_cap;
    for (sz i = 0; i < namelen; i++)
        p->name[i] = path.dat[i];

    struct byte_view bv = byte_view_new(buf, bb.len);
    struct result lr = proc_load_flat(p, bv);
    if (lr.is_error) {
        shell_writef(sess, STR("spawn: load failed: %hu\n"), lr.code);
        proc_free(p);
        return;
    }

    /* Set RUNNING before enqueue: sched_create_user_task enqueues
     * immediately, another CPU could schedule the child before returning.
     */
    proc_set_state(p, PROC_STATE_RUNNING);

    struct result tr =
        sched_create_user_task(p, (ptr) p->va_code_base, SCHED_PRIO_NORMAL);
    if (tr.is_error) {
        shell_writef(sess, STR("spawn: task creation failed: %hu\n"), tr.code);
        proc_set_state(p, PROC_STATE_ZOMBIE);
        proc_free(p);
        return;
    }
    shell_writef(sess, STR("spawned pid %hu\n"), (u32) p->pid);
}

struct ps_ctx {
    struct shell_session *sess;
    sz n;
};

static void ps_cb(struct proc *p, void *ctx_ptr)
{
    struct ps_ctx *ctx = ctx_ptr;
    struct str state = proc_state_name(p->state);
    sz nlen = 0;
    while (nlen < sizeof(p->name) && p->name[nlen])
        nlen++;
    shell_writef(ctx->sess, STR("  %hu  %-8s  %s\n"), (u32) p->pid, state,
                 str_new(p->name, nlen));
    ctx->n++;
}

static void cmd_ps(struct shell_session *sess)
{
    /* Kernel tasks (always present). */
    shell_write(sess, STR("tasks:\n"));
    shell_write(sess, STR("  ID    State     CPU time      Entry\n"));
    struct tasks_ctx tctx = {.sess = sess, .n = 0};
    sched_for_each_task(tasks_cb, &tctx);
    shell_writef(sess, STR("  (%ld kernel tasks)\n"), (i64) tctx.n);

    /* User processes (spawned via 'spawn'). */
    shell_write(sess, STR("processes:\n"));
    shell_write(sess, STR("  PID   State     Name\n"));
    struct ps_ctx ctx = {.sess = sess, .n = 0};
    proc_for_each(ps_cb, &ctx);
    if (ctx.n == 0)
        shell_write(sess, STR("  (none)\n"));
}

static void cmd_kill(struct shell_session *sess, struct str pid_str)
{
    if (pid_str.len == 0) {
        shell_write(sess, STR("kill: missing pid\n"));
        return;
    }

    /* Parse decimal PID with overflow check. */
    u32 acc = 0;
    for (sz i = 0; i < pid_str.len; i++) {
        if (pid_str.dat[i] < '0' || pid_str.dat[i] > '9') {
            shell_writef(sess, STR("kill: invalid pid \"%s\"\n"), pid_str);
            return;
        }
        u32 digit = (u32) (pid_str.dat[i] - '0');
        if (acc > (U16_MAX - digit) / 10) {
            shell_writef(sess, STR("kill: pid out of range\n"));
            return;
        }
        acc = acc * 10 + digit;
    }
    u16 pid = (u16) acc;

    struct proc *p = proc_find(pid);
    if (!p) {
        shell_writef(sess, STR("kill: pid %hu not found\n"), (u32) pid);
        return;
    }

    p->exit_code = -1;
    if (p->task) {
        p->task->proc = NULL;
        sched_set_task_state(p->task, TD_STATE_TERMINATING);
    }
    p->task = NULL;
    bool is_orphan = proc_is_missing_or_zombie(p->parent_pid);
    proc_set_state(p, PROC_STATE_ZOMBIE);
    if (is_orphan)
        proc_free(p);
    else
        proc_notify_parent(p);

    shell_writef(sess, STR("killed pid %hu\n"), (u32) pid);
}

static void cmd_echo(struct shell_session *sess, struct str rest)
{
    if (rest.len > 0)
        shell_write(sess, rest);
    shell_write(sess, STR("\n"));
}

static void cmd_uptime(struct shell_session *sess)
{
    u64 ms = time_current_ms().ms;
    u64 secs = ms / 1000;
    u64 mins = secs / 60;
    u64 hours = mins / 60;
    shell_writef(sess, STR("up %lu:%02lu:%02lu (%lu ms)\n"), hours, mins % 60,
                 secs % 60, ms);
}

#if CONFIG_SEMIHOSTING
#include <semihost.h>

static void cmd_shutdown(struct shell_session *sess)
{
    shell_write(sess, STR("shutting down...\n"));
    semihost_exit(0);
}
#endif

/* Dispatcher */

void shell_exec(struct shell_session *sess, struct str cmd, struct arena tmp)
{
    assert(sess);

    struct str rest = cmd;
    struct str verb = shell_next_token(&rest);

    if (verb.len == 0)
        return;

    if (str_is_equal(verb, STR("help"))) {
        cmd_help(sess);
    } else if (str_is_equal(verb, STR("ls"))) {
        cmd_ls(sess, shell_next_token(&rest));
    } else if (str_is_equal(verb, STR("cat"))) {
        cmd_cat(sess, shell_next_token(&rest));
    } else if (str_is_equal(verb, STR("netstat"))) {
        cmd_netstat(sess);
    } else if (str_is_equal(verb, STR("mem"))) {
        cmd_mem(sess);
    } else if (str_is_equal(verb, STR("tcpstats"))) {
        cmd_tcpstats(sess);
#if CONFIG_VIRTIO_BLK
    } else if (str_is_equal(verb, STR("blkinfo"))) {
        cmd_blkinfo(sess);
#endif
    } else if (str_is_equal(verb, STR("spawn"))) {
        cmd_spawn(sess, shell_next_token(&rest));
    } else if (str_is_equal(verb, STR("ps"))) {
        cmd_ps(sess);
    } else if (str_is_equal(verb, STR("kill"))) {
        cmd_kill(sess, shell_next_token(&rest));
    } else if (str_is_equal(verb, STR("echo"))) {
        cmd_echo(sess, rest);
    } else if (str_is_equal(verb, STR("uptime"))) {
        cmd_uptime(sess);
#if CONFIG_SEMIHOSTING
    } else if (str_is_equal(verb, STR("shutdown"))) {
        cmd_shutdown(sess);
#endif
    } else if (str_is_equal(verb, STR("ping"))) {
        cmd_ping(sess, shell_next_token(&rest), tmp);
#if CONFIG_NET_UDP
    } else if (str_is_equal(verb, STR("nslookup"))) {
        cmd_nslookup(sess, shell_next_token(&rest), tmp);
#endif
#if CONFIG_RAMFS_WRITABLE
    } else if (str_is_equal(verb, STR("write"))) {
        struct str path = shell_next_token(&rest);
        /* Everything remaining after the path is the content. */
        struct str content = rest;
        /* Strip leading whitespace from content. */
        while (content.len > 0 &&
               (content.dat[0] == ' ' || content.dat[0] == '\t')) {
            content.dat++;
            content.len--;
        }
        cmd_write(sess, path, content);
    } else if (str_is_equal(verb, STR("rm"))) {
        cmd_rm(sess, shell_next_token(&rest));
    } else if (str_is_equal(verb, STR("mkdir"))) {
        cmd_mkdir(sess, shell_next_token(&rest));
#endif
    } else {
        shell_writef(sess, STR("unknown command: \"%s\" (try: help)\n"), verb);
    }
}
