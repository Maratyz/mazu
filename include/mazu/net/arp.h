/* SPDX-License-Identifier: MIT */
/* Address Resolution Protocol (ARP) implementation for IPv4 address
 * resolution.
 */

#ifndef MAZU_NET_ARP_H
#define MAZU_NET_ARP_H

#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/netorder.h>

#define ARP_OPCODE_REQUEST 1
#define ARP_OPCODE_REPLY 2

#define ARP_HTYPE_ETHERNET 1
/* RFC 5227 defend rate limit: send at most one defensive ARP per 10 seconds. */
#define ARP_DEFEND_INTERVAL_MS (10UL * 1000UL)

struct arp_header {
    net_u16 htype;
    net_u16 ptype;
    u8 hlen;
    u8 plen;
    net_u16 opcode;
    u8 payload[];
} __packed;

static_assert(sizeof(struct arp_header) == 8, "unexpected arp_header size");

/* Broadcast an ARP REQUEST packet from the device 'netdev'. The 'dest_ip' is
 * the IPv4 address being resolved to a MAC address.
 */
struct result arp_send_request(struct ipv4_addr dest_ip,
                               struct netdev *netdev,
                               struct send_buf sb,
                               struct arena tmp);

/* Lookup a MAC address associated with the given IPv4 address in the ARP
 * table. Returns either the MAC address or nothing. The read path is lockless;
 * expired entries are treated as misses and are reclaimed later by update or
 * iteration paths.
 */
struct option_mac_addr arp_lookup_mac_addr(struct ipv4_addr ip_addr);

/* Handle an ARP packet. Call this function on any incoming packets that were
 * identified as ARP packets. It will update the ARP table and reply to the
 * sender using the same device that the ARP packet was received on.
 *
 * NOTE: This function WILL NOT check if the destination MAC address in the ARP
 * packet belongs to this host. The caller of this function which receives the
 * packet should ensure that it's correctly destined for this host.
 */
struct result arp_handle_packet(struct input_packet *pkt,
                                struct send_buf sb,
                                struct arena tmp);

/* Send a gratuitous ARP reply (unsolicited) to announce the host IP->MAC
 * binding.
 * Call on boot and on IP address changes.
 */
struct result arp_send_gratuitous(struct netdev *netdev,
                                  struct send_buf sb,
                                  struct arena tmp);

/* ARP table entry exposed for iteration. */
struct arp_entry_info {
    struct ipv4_addr ip_addr;
    struct mac_addr mac_addr;
    u64 age_ms; /* milliseconds since entry was added */
};

typedef void (*arp_iter_cb_t)(struct arp_entry_info info, void *ctx);

/* Iterate all valid (non-expired) entries in the ARP table, calling 'cb' for
 * each.
 */
void arp_for_each(arp_iter_cb_t cb, void *ctx);

#endif /* MAZU_NET_ARP_H */
