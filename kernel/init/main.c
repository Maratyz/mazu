/* SPDX-License-Identifier: MIT */
#include <fdt.h>
#include <isr.h>
#include <mazu/arena.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/buddy.h>
#include <mazu/byte.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/klog.h>
#include <mazu/kvalloc.h>
#include <mazu/net/ip_addr.h>
#include <mazu/paging.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/proc.h>
#include <mazu/ramfs.h>
#include <mazu/sched.h>
#include <mazu/selftest.h>
#include <mazu/string.h>
#include <mazu/time.h>
#include <mazu/vfs.h>
#if CONFIG_NET_TCP
#include <mazu/net/arp.h>
#if CONFIG_DHCP
#include <mazu/net/dhcp.h>
#endif
#include <mazu/net/ethernet.h>
#include <mazu/net/icmp.h>
#include <mazu/net/ip.h>
#include <mazu/net/netdev.h>
#include <mazu/net/tcp.h>
#endif
#if CONFIG_VIRTIO_BLK
#include <mazu/blkdev.h>
#endif

#include "../fs/archive.h"
#include "../fs/devfs.h"
#include "../fs/netfs.h"
#include "../fs/procfs.h"
#include "../rtcfg.h"
#if CONFIG_VIRTIO_BLK
#include "../fs/bcache.h"
#include "../fs/sfs.h"
#endif

extern char _rootfs_archive_start[];
extern char _rootfs_archive_end[];

#if CONFIG_NET_TCP
/* User-space task entry points (declared here to avoid kernel→user includes).
 */
extern void shell_init(void);
extern struct result web_listen(struct ipv4_addr ip_addr,
                                u16 port,
                                struct ram_fs_node *root);
#endif

/* Provided by arch/$(ARCH)/arch.c: */
void arch_init(void);
#if CONFIG_NET_TCP
struct result arch_net_probe(void);
#endif

#if CONFIG_VIRTIO_BLK
struct result virtio_blk_probe(void);
#endif

#if CONFIG_SMP
void smp_boot_secondaries(void);
#endif

/* Kernel initialization */

static void mem_init(void)
{
    assert(CONFIG_KERN_DYN_PADDR > CONFIG_KERN_BASE_PADDR &&
           CONFIG_KERN_DYN_PADDR - CONFIG_KERN_BASE_PADDR ==
               CONFIG_KERN_DYN_VADDR - CONFIG_KERN_BASE_VADDR);
    struct addr_mapping code_addrs = {
        .vbase = CONFIG_KERN_BASE_VADDR,
        .pbase = CONFIG_KERN_BASE_PADDR,
        .len = CONFIG_KERN_DYN_PADDR - CONFIG_KERN_BASE_PADDR,
    };
    struct addr_mapping dyn_addrs = {
        .vbase = CONFIG_KERN_DYN_VADDR,
        .pbase = CONFIG_KERN_DYN_PADDR,
        .len = CONFIG_KERN_DYN_LEN,
    };

    struct byte_array dyn = paging_init(code_addrs, dyn_addrs);

    struct result res = kvalloc_init(dyn);
    assert(!res.is_error);

    pr_info(STR("Initialized paging and virtual memory allocator\n"));
}

static struct ram_fs *ram_fs_init(void)
{
    struct alloc rfs_alloc = {
        .a_ptr = NULL,
        .alloc = kvalloc_alloc_wrapper,
        .free = kvalloc_free_wrapper,
    };

    struct ram_fs *rfs = ram_fs_new(rfs_alloc);
    assert(rfs);

    pr_info(STR("Initialized RAM file system\n"));

    return rfs;
}

#if CONFIG_NET_TCP
static void net_init(struct runtime_config *cfg, struct arena arn)
{
    struct ipv4_addr host_ip = option_ipv4_addr_checked(cfg->host_ip);
    struct ipv4_addr default_gateway_ip =
        option_ipv4_addr_checked(cfg->default_gateway_ip);
    struct ipv4_addr local_ip = option_ipv4_addr_checked(cfg->local_ip);
    struct ipv4_addr local_ip_mask =
        option_ipv4_addr_checked(cfg->local_ip_mask);

    /* This is the default route for all IP addresses that are outside of the
     * local network (see below).
     */
    ipv4_route_add((struct ipv4_route_entry) {
        .dest = IPV4_ADDR_ANY,
        .mask = IPV4_ADDR_ANY,
        .gateway = default_gateway_ip,
        .interface = host_ip,
    });

    /* This is a route to all local IP addresses. Other hosts on the virtual
     * network with this host and the VM host can be reached this way.
     */
    ipv4_route_add((struct ipv4_route_entry) {
        .dest = local_ip,
        .mask = local_ip_mask,
        .gateway = host_ip,
        .interface = host_ip,
    });

    /* Initialize the 'netdev' subsystem. */
    netdev_set_default_ip_addr(host_ip);
    assert(!netdev_init_input_queue().is_error);

    pr_info(STR("Initialized IP networking: host=%s default_gateway=%s "
                "local=%s/%ld\n"),
            ipv4_addr_format(host_ip, &arn),
            ipv4_addr_format(default_gateway_ip, &arn),
            ipv4_addr_format(local_ip, &arn),
            ipv4_mask_prefix_length(local_ip_mask));
}
#endif /* CONFIG_NET_TCP */

static void print_hello_message(struct ram_fs *rfs, struct arena tmp)
{
    assert(rfs);

    struct result_ram_fs_node node_res = ram_fs_open(rfs->root, STR("/hello."
                                                                    "txt"));
    assert(!node_res.is_error);
    struct ram_fs_node *node = result_ram_fs_node_checked(node_res);
    struct byte_buf bbuf =
        byte_buf_from_array(byte_array_from_arena(500, &tmp));
    struct result_sz read_res = ram_fs_read(node, &bbuf, 0);
    assert(!read_res.is_error);

    print_str(str_from_byte_buf(bbuf));

    struct str_buf sbuf = str_buf_from_arena(&tmp, 128);
    print_fmt(sbuf, STR("Commit: %s\n"), STR(GIT_COMMIT));
}

/* Defined below: */
static void main(struct ram_fs *rfs, struct runtime_config *rtcfg);

/* Stack-protector canary initialization (must be before any protected fn). */
#if CONFIG_STACK_PROTECTOR
extern void stack_chk_init(void);
#endif

__noreturn void kernel_init(void)
{
#if CONFIG_STACK_PROTECTOR
    stack_chk_init();
#endif

#if CONFIG_SMP
    /* Set BSP's physical hartid before arch_init, because interrupt_init() uses
     * get_hartid() to compute the PLIC context.  OpenSBI may pick any hart as
     * the boot hart (not necessarily hart 0).
     */
    {
        extern u64 _bsp_hartid;
        pcpu_array[0].hartid = (u32) _bsp_hartid;
    }
#endif

    /* Parse FDT before arch_init so board_info is available for UART/PLIC.
     * Failure is non-fatal: consumers fall back to QEMU-virt defaults.
     */
    fdt_parse((void *) (uptr) _fdt_addr, &board_info);

    /* arch_init() initializes the serial port / UART (so printk works
     * immediately after), the interrupt controller, and CSR hardening.
     */
    arch_init();

    time_init();

    mem_init();

    initgraph_run(INIT_FLAG_PRIMARY);

    /* Start the klog ring buffer (tee mode).  After this point, printk
     * output is captured in the ring buffer AND sent to the original
     * console (UART/semihosting) synchronously.  The ring provides
     * backing store for /api/klog and future /dev/kmesg.
     */
    klog_init();

#if CONFIG_SMP
    smp_boot_secondaries();
#endif

    selftest_check_and_run();

    struct byte_array tmp_ba =
        option_byte_array_checked(kvalloc_alloc(0x2000, 64));
    struct arena tmp = arena_new(tmp_ba);

    struct ram_fs *rfs = ram_fs_init();
    assert(rfs);

    struct byte_view rootfs_archive = byte_view_new(
        _rootfs_archive_start, _rootfs_archive_end - _rootfs_archive_start);
    assert(!archive_extract(rootfs_archive, rfs).is_error);

    vfs_init(rfs);

    /* Mount Plan 9-style synthetic filesystems. */
    {
        struct result sfs_res;
        sfs_res = vfs_mount(STR("/dev"), devfs_vfs_ops(), NULL);
        assert(!sfs_res.is_error);
        sfs_res = vfs_mount(STR("/proc"), procfs_vfs_ops(), NULL);
        assert(!sfs_res.is_error);
        sfs_res = vfs_mount(STR("/net"), netfs_vfs_ops(), NULL);
        assert(!sfs_res.is_error);
        pr_info(STR("VFS: mounted /dev, /proc, /net\n"));
    }

#if CONFIG_RAMFS_WRITABLE
    {
        struct alloc tmpfs_alloc = {
            .a_ptr = NULL,
            .alloc = kvalloc_alloc_wrapper,
            .free = kvalloc_free_wrapper,
        };
        struct ram_fs *tmpfs = ram_fs_new(tmpfs_alloc);
        assert(tmpfs);
        struct result tmp_mnt_res =
            vfs_mount(STR("/tmp"), ramfs_vfs_ops(), tmpfs);
        assert(!tmp_mnt_res.is_error);
        pr_info(STR("VFS: mounted writable tmpfs at /tmp\n"));
    }
#endif

    struct result_runtime_config rtcfg_res =
        rtcfg_read_config(rfs, STR("/config.txt"), tmp);
    assert(!rtcfg_res.is_error);
    struct runtime_config *rtcfg = result_runtime_config_checked(rtcfg_res);
    assert(rtcfg);

#if CONFIG_NET_TCP
    net_init(rtcfg, tmp);
    assert(!arch_net_probe().is_error);

    /* Send gratuitous ARP to announce the host's presence on the network. */
    {
        struct netdev *nd =
            netdev_lookup_ip_addr(option_ipv4_addr_checked(rtcfg->host_ip));
        if (nd) {
            struct byte_array garp_tmp_ba =
                option_byte_array_checked(kvalloc_alloc(0x1000, 64));
            struct byte_array garp_sb_ba =
                option_byte_array_checked(kvalloc_alloc(0x2000, 64));
            struct arena garp_tmp = arena_new(garp_tmp_ba);
            struct send_buf garp_sb = send_buf_new(arena_new(garp_sb_ba));
            arp_send_gratuitous(nd, garp_sb, garp_tmp);
            kvalloc_free(garp_sb_ba);
            kvalloc_free(garp_tmp_ba);
            netdev_put(nd);
        }
    }
#endif

#if CONFIG_VIRTIO_BLK
    {
        struct result blk_res = virtio_blk_probe();
        if (blk_res.is_error)
            pr_info(
                STR("virtio-blk: probe failed (no block "
                    "device)\n"));
        else if (global_blkdev) {
            static struct bcache disk_bcache;
            struct result bc_res =
                bcache_init(&disk_bcache, global_blkdev, 256);
            if (!bc_res.is_error) {
                /* Read sector 0 to check for SFS magic. */
                struct buf *sb = bcache_read(&disk_bcache, 0);
                bool needs_format = true;
                if (sb) {
                    u32 magic;
                    memcpy(&magic, sb->data, sizeof(magic));
                    if (magic == SFS_MAGIC)
                        needs_format = false;
                    bcache_release(sb);
                }
                if (needs_format) {
                    pr_info(STR("sfs: formatting disk (%lu sectors)\n"),
                            global_blkdev->capacity);
                    sfs_format(&disk_bcache, (u32) global_blkdev->capacity);
                }
                struct result_sfs mnt = sfs_mount(&disk_bcache);
                if (!mnt.is_error) {
                    struct sfs *disk_fs = result_sfs_checked(mnt);
                    struct result vfs_res =
                        vfs_mount(STR("/disk"), sfs_vfs_ops(), disk_fs);
                    if (!vfs_res.is_error)
                        pr_info(STR("VFS: mounted SFS at /disk\n"));
                }
            }
        }
    }
#endif

#if CONFIG_DHCP
    {
        struct netdev *dev =
            netdev_lookup_ip_addr(option_ipv4_addr_checked(rtcfg->host_ip));
        struct ipv4_addr dhcp_ip;
        if (dev) {
            struct netdev_info info;
            netdev_get_info(dev, &info);
            struct result dhcp_res = dhcp_boot_acquire(dev, &dhcp_ip, tmp);
            if (!dhcp_res.is_error) {
                if (ipv4_addr_is_equal(info.ip_addr, IPV4_ADDR_ANY)) {
                    /* No static IP configured — adopt the lease. */
                    netdev_set_ip_addr(dev, dhcp_ip);
                    rtcfg->host_ip = option_ipv4_addr_ok(dhcp_ip);
                    netdev_set_default_ip_addr(dhcp_ip);
                    pr_info(STR("DHCP: adopted lease %s\n"),
                            ipv4_addr_format(dhcp_ip, &tmp));
                } else {
                    pr_info(STR("DHCP: lease %s, keeping static IP %s\n"),
                            ipv4_addr_format(dhcp_ip, &tmp),
                            ipv4_addr_format(info.ip_addr, &tmp));
                }
            } else {
                pr_info(STR("DHCP: timed out, using static IP\n"));
            }
            netdev_put(dev);
        }
    }
#endif

    proc_init();

#if CONFIG_NET_TCP
    shell_init();
#endif

    print_hello_message(rfs, tmp);

    kvalloc_free(tmp_ba);

    main(rfs, rtcfg);

    halt_execution();
}

/* Main loop with tasks */

#if CONFIG_NET_TCP
static void task_net_ping(void *ctx_ptr __unused)
{
    struct sched_domain *dom = sched_get_kernel_domain(SCHED_DOMAIN_SYS);
    if (dom)
        sched_domain_attach(dom, sched_current_task());

    /* Send a few pings to the default gateway to exercise the ICMP path
     * and verify network reachability at boot time.
     */
    struct ping_stats stats =
        icmp_ping(ipv4_addr_new(192, 168, 100, 1), 0xcafe, 3);
    if (stats.received > 0)
        pr_info(STR("Ping: %hu/%hu received, rtt min/avg/max = %lu/%lu/%lu "
                    "us\n"),
                stats.received, stats.sent, stats.min_rtt_us, stats.avg_rtt_us,
                stats.max_rtt_us);
    else
        pr_info(STR("Ping: %hu sent, 0 received\n"), stats.sent);
}

static void task_tcp_poll_retransmit(void *ctx_ptr __unused)
{
    struct sched_domain *dom = sched_get_kernel_domain(SCHED_DOMAIN_SYS);
    if (dom)
        sched_domain_attach(dom, sched_current_task());

    struct arena tmp =
        arena_new(option_byte_array_checked(kvalloc_alloc(0x2000, 64)));

    while (true) {
        struct result res = tcp_poll_retransmit(tmp);
        if (res.is_error)
            pr_debug(STR("Error in TCP retransmission: %hu\n"), res.code);

        /* Sleep until the next TCP timer fires rather than a fixed poll.
         * tcp_poll_retransmit() rebuilds the min-heap so tcp_timer_next_ms()
         * is O(1).  Cap at TCP_DELAYED_ACK_MS (200ms) so that delayed ACKs
         * armed by the RX path between poll cycles are serviced within the
         * RFC 1122 §4.2.3.2 maximum of 500ms.
         */
        u64 now_ms = time_current_ms().ms;
        u64 next_ms = tcp_timer_next_ms();
        u64 wait_ms = (next_ms > now_ms) ? (next_ms - now_ms) : 0;
        if (wait_ms > 200)
            wait_ms = 200;
        sleep_ms(time_ms_new(wait_ms));
    }
}

struct task_ctx_net_receive {
    struct arena tmp;
    struct send_buf sb;
};

static void task_net_receive(void *ctx_ptr)
{
    assert(ctx_ptr);
    struct task_ctx_net_receive *ctx = ctx_ptr;

    /* No domain: this is the sole inbound packet pump.  Throttling it
     * throttles every protocol (ARP, ICMP, TCP) and every domain that
     * depends on network I/O, including the WEB domain.
     */

    while (true) {
        struct input_packet *in_packet;
        while ((in_packet = netdev_get_input()) != NULL) {
            if (in_packet->n_failed_to_handle > 5) {
                netdev_release_input(in_packet);
                continue;
            }

            send_buf_clear(&ctx->sb);

            struct result res;
            switch (in_packet->proto) {
            case NETDEV_PROTO_ARP:
                res = arp_handle_packet(in_packet, ctx->sb, ctx->tmp);
                break;
            case NETDEV_PROTO_IPV4:
                res = ipv4_handle_packet(in_packet, ctx->sb, ctx->tmp);
                break;
            default:
                pr_info(STR("Received packet with unknown protocol 0x%hx. "
                            "Dropping ...\n"),
                        in_packet->proto);
                res = result_ok();
                break;
            }

            if (res.is_error) {
                pr_warn(STR("Failed to handle packet 0x%lx. Total attempts: "
                            "%ld\n"),
                        in_packet, in_packet->n_failed_to_handle);
                in_packet->n_failed_to_handle++;
            } else {
                netdev_release_input(in_packet);
            }
        }

        sleep_ms(time_ms_new(10));
    }
}

struct task_ctx_web_listen {
    struct ipv4_addr addr;
    u16 port;
    struct ram_fs_node *root;
};

static void task_web_listen(void *ctx_ptr)
{
    assert(ctx_ptr);
    struct task_ctx_web_listen *ctx = ctx_ptr;

    struct sched_domain *dom = sched_get_kernel_domain(SCHED_DOMAIN_WEB);
    if (dom)
        sched_domain_attach(dom, sched_current_task());

    web_listen(ctx->addr, ctx->port, ctx->root);
}
#endif /* CONFIG_NET_TCP */

static void main(struct ram_fs *rfs, struct runtime_config *rtcfg)
{
    assert(rfs);
    assert(rtcfg);

#if CONFIG_NET_TCP
    /* task_net_ping is intentionally finite; exits after 5 pings. */
    sched_create_task_prio(task_net_ping, NULL, SCHED_PRIO_NORMAL);

    /* These three tasks must run forever; an unexpected return is a kernel bug.
     */
    sched_create_task_noreturn_prio(task_tcp_poll_retransmit, NULL,
                                    SCHED_PRIO_HIGH);

    static struct task_ctx_net_receive net_rx_ctx;
    net_rx_ctx.tmp =
        arena_new(option_byte_array_checked(kvalloc_alloc(0x2000, 64)));
    net_rx_ctx.sb = send_buf_new(
        arena_new(option_byte_array_checked(kvalloc_alloc(0x4000, 64))));
    sched_create_task_noreturn_prio(task_net_receive, &net_rx_ctx,
                                    SCHED_PRIO_HIGH);

    struct result_ram_fs_node web_res = ram_fs_open(rfs->root, STR("/web/"));
    assert(!web_res.is_error);
    struct ram_fs_node *web_dir = result_ram_fs_node_checked(web_res);
    assert(web_dir->type == RAM_FS_TYPE_DIR);
    static struct task_ctx_web_listen web_ctx;
    web_ctx.addr = option_ipv4_addr_checked(rtcfg->host_ip);
    web_ctx.port = 80;
    web_ctx.root = web_dir;
    sched_create_task_noreturn_prio(task_web_listen, &web_ctx,
                                    SCHED_PRIO_NORMAL);

    while (true)
        sleep_ms(time_ms_new(1000));
#else
    pr_info(
        STR("Networking disabled by config; parking init "
            "task\n"));
    while (true)
        sleep_ms(time_ms_new(TIME_MS_MAX));
#endif
}
