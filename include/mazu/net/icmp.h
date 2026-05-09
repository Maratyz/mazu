/* SPDX-License-Identifier: MIT */
/* Internet Control Message Protocol (ICMP) implementation (version 4 only). */

#ifndef MAZU_NET_ICMP_H
#define MAZU_NET_ICMP_H

#include <mazu/byte.h>
#include <mazu/error.h>
#include <mazu/net/ip.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/send_buf.h>

struct result icmpv4_send_echo(struct ipv4_addr dest_addr,
                               u16 ident,
                               u16 seq,
                               struct send_buf sb,
                               struct arena arn);

struct result icmpv4_handle_message(struct ipv4_addr src_addr,
                                    struct byte_view message,
                                    struct send_buf sb,
                                    struct arena arn);

/* Ping statistics returned by icmp_ping(). */
struct ping_stats {
    u16 sent;
    u16 received;
    u64 min_rtt_us;
    u64 avg_rtt_us;
    u64 max_rtt_us;
};

/* Send 'count' ICMP echo requests to 'dest' at ~1s intervals and collect
 * RTT statistics.  Blocks until all pings complete or time out (2s per ping).
 * Allocates temporary buffers internally via kvalloc.
 */
struct ping_stats icmp_ping(struct ipv4_addr dest, u16 ident, u16 count);

#endif /* MAZU_NET_ICMP_H */
