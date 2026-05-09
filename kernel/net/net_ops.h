/* SPDX-License-Identifier: MIT */
/* Network operations vtable for testability and future per-process isolation.
 *
 * TCP code calls through a module-internal net_ops pointer instead of invoking
 * timing and IP functions directly.  The default instance wires to the real
 * platform functions.  Kernel selftests (compiled into the same translation
 * unit) can swap in a mock for unit testing.
 */

#ifndef MAZU_NET_NET_OPS_H
#define MAZU_NET_NET_OPS_H

#include <mazu/base.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/send_buf.h>
#include <mazu/time.h>

struct net_ops {
    /* Timing */
    struct time_ms (*current_ms)(void);
    u64 (*rdtime)(void);
    u64 (*timebase_freq)(void);

    /* IP layer */
    struct result (*ip_send)(struct ipv4_addr dest,
                             u8 proto,
                             struct send_buf sb,
                             struct arena tmp);
    struct result_ipv4_addr (*ip_route_addr)(struct ipv4_addr dest);
    struct result_sz (*ip_route_mtu)(struct ipv4_addr dest);
};

/* Default instance backed by real platform functions.
 * Initialized by tcp_init() before first use.
 */
extern const struct net_ops net_ops_default;

#endif /* MAZU_NET_NET_OPS_H */
