/* SPDX-License-Identifier: MIT */
/* Minimal mDNS (Multicast DNS, RFC 6762) responder.
 * Answers A-record queries for the compile-time hostname "mazu.local".
 * Compiled only when CONFIG_NET_MDNS is enabled.
 */

#ifndef MAZU_NET_MDNS_H
#define MAZU_NET_MDNS_H

#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/send_buf.h>

/* mDNS multicast group (RFC 6762 §5). */
#define MDNS_PORT 5353
#define MDNS_MULTICAST_0 224
#define MDNS_MULTICAST_1 0
#define MDNS_MULTICAST_2 0
#define MDNS_MULTICAST_3 251

/* Compile-time hostname answered by this responder (without trailing dot). */
#define MDNS_HOSTNAME "mazu"
#define MDNS_DOMAIN "local"

/* Called by udp.c when a UDP datagram arrives on port 5353.
 * @recv_dev : interface query arrived on (used for source IP and send).
 * @src_ip'  : querier's IP address (for unicast responses when QU is set).
 * @payload  : raw DNS message (no UDP header).
 * @sb       : caller-provided send buffer for the response.
 */
struct result mdns_handle_query(struct netdev *recv_dev,
                                struct ipv4_addr src_ip,
                                struct byte_view payload,
                                struct send_buf sb,
                                struct arena tmp);

#endif /* MAZU_NET_MDNS_H */
