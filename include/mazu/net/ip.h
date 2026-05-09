/* SPDX-License-Identifier: MIT */
/* Internet protocol (IP) implementation (only version 4). */

#ifndef MAZU_NET_IP_H
#define MAZU_NET_IP_H

#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/send_buf.h>
#include <mazu/time.h>

#define IPV4_PROTOCOL_ICMP 1
#define IPV4_PROTOCOL_TCP 6
#define IPV4_PROTOCOL_UDP 17

struct ipv4_route_entry {
    /* Destination IP address (not necessarily the same network as this host).
     */
    struct ipv4_addr dest;
    /* Mask to compare a given destination address to the 'dest' field. */
    struct ipv4_addr mask;
    /* IP address of the host on this network to send the datagram to. */
    struct ipv4_addr gateway;
    /* IP address of the interface (netdev) to send the datagram from. */
    struct ipv4_addr interface;
};

struct result ipv4_handle_packet(struct input_packet *pkt,
                                 struct send_buf sb,
                                 struct arena tmp);

/* 'proto' is one of the 'IPV4_PROTOCOL_*' constants. */
struct result ipv4_send_packet(struct ipv4_addr dest_ip,
                               u8 proto,
                               struct send_buf sb,
                               struct arena arn);

/* Variant of ipv4_send_packet() that uses an explicit source address in the
 * IPv4 header. Callers are responsible for ensuring 'src_ip' matches the
 * routed egress interface for 'dest_ip'.
 */
struct result ipv4_send_packet_from(struct ipv4_addr src_ip,
                                    struct ipv4_addr dest_ip,
                                    u8 proto,
                                    struct send_buf sb,
                                    struct arena arn);

net_u16 internet_checksum(struct byte_view data);
net_u16 internet_checksum_iterate(net_u16 checksum, struct byte_view data);
net_u16 internet_checksum_add(net_u16 a, net_u16 b);
net_u16 internet_checksum_finalize(net_u16 checksum);

/* Build and prepend an IPv4 header onto *sb in place.  Used by UDP broadcast
 * (which bypasses the routing table) and internally by ipv4_send_packet. 'ttl'
 * is the IP Time-To-Live field; pass 64 for normal packets, 255 for mDNS
 * responses (RFC 6762 §11 requires TTL=255 on multicast DNS traffic).
 * 'frag_flags' is written into the fragment_offset field; pass 0 normally, or
 * IPV4_FLAG_DF (0x4000) to set the Don't Fragment bit (PMTUD).
 */
struct result ipv4_prepend_header(struct ipv4_addr src_ip,
                                  struct ipv4_addr dest_ip,
                                  u8 proto,
                                  u8 ttl,
                                  u16 frag_flags,
                                  struct send_buf *sb);

#define IPV4_FLAG_DF 0x4000 /* Don't Fragment bit in fragment_offset field */

#if CONFIG_PMTUD
/* Update the PMTU cache for a destination.  Clamps to minimum 576. */
u16 pmtu_update(struct ipv4_addr dest, u16 new_mtu);

/* Lookup cached PMTU for a destination. Returns 0 if no cache entry or if the
 * cached entry has expired. Read path is lockless; expiry cleanup is lazy and
 * handled by the update path.
 */
u16 pmtu_lookup(struct ipv4_addr dest);
#endif

struct result ipv4_route_add(struct ipv4_route_entry ent);

/* Return the outward-facing IP address of the interface that is used to reach
 * 'dest_ip'. This function performs a lookup in the routing table and returns
 * the 'interface' field from the entry matching 'dest_ip' if one exists.
 *
 * TCP (and UDP and possibly others) computes an end-to-end checksum that
 * includes fields from the IP header. The TCP implementation uses this function
 * to find out what the source IP address field will be in the outgoing
 * datagram. It needs to know this to compute the end-to-end checksum.
 */
struct result_ipv4_addr ipv4_route_interface_addr(struct ipv4_addr dest_ip);

/* Return the device MTU for the interface that will be used to route outgoing
 * traffic destined for 'dest_ip'.
 */
struct result_sz ipv4_route_mtu(struct ipv4_addr dest_ip);

#endif /* MAZU_NET_IP_H */
