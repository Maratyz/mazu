/* SPDX-License-Identifier: MIT */
#include <mazu/byte.h>
#include <mazu/net/arp.h>
#include <mazu/net/icmp.h>
#include <mazu/net/ip.h>
#include <mazu/net/netdev.h>
#include <mazu/net/tcp.h>
#include <mazu/spinlock.h>
#if CONFIG_NET_UDP
#include <mazu/net/udp.h>
#endif

struct ipv4_header {
#if SYSTEM_BYTE_ORDER == NET_BYTE_ORDER
    u8 version : 4;
    u8 ihl : 4; /* IHL: length of the IPv4 header in 32-bit words */
#else
    u8 ihl : 4; /* IHL: length of the IPv4 header in 32-bit words */
    u8 version : 4;
#endif
    u8 ds_ecn;
    net_u16 total_length; /* Length of IPv4 datagram in bytes (incl. header) */
    net_u16 ident;
    net_u16 fragment_offset;
    u8 ttl;
    u8 protocol;
    net_u16 checksum;
    struct ipv4_addr src_addr;
    struct ipv4_addr dest_addr;
} __packed;

static_assert(sizeof(struct ipv4_header) == 20, "unexpected ipv4_header size");

/* Internet checksum

 * Internet checksum input is in network byte order. RFC 1071 arithmetic makes
 * the folded result valid regardless of host endianness.
 */
net_u16 internet_checksum_iterate(net_u16 checksum, struct byte_view data)
{
    /* Refer to RFC 1071 for this algorithm.
     * https://datatracker.ietf.org/doc/html/rfc1071
     */
    u32 sum = checksum.inner;
    const u8 *ptr = byte_view_ptr(data);
    sz len = data.len;

    /* Byte-at-a-time accumulation avoids unaligned u16 loads which trap
     * on RISC-V harts without misaligned-access support.
     */
    while (len > 1) {
        sum += (u16) ptr[0] | ((u16) ptr[1] << 8);
        ptr += 2;
        len -= 2;
    }

    if (len > 0)
        sum += ptr[0];

    /* Fold the 32-bit sum into the 16-bit range. */
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return (net_u16) {sum};
}

net_u16 internet_checksum_add(net_u16 a, net_u16 b)
{
    u32 sum = (u32) a.inner + (u32) b.inner;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (net_u16) {sum};
}

net_u16 internet_checksum_finalize(net_u16 sum)
{
    return (net_u16) {~sum.inner};
}

net_u16 internet_checksum(struct byte_view data)
{
    return internet_checksum_finalize(
        internet_checksum_iterate(net_u16_from_u16(0), data));
}

static inline bool ipv4_checksum_is_ok(struct ipv4_header *hdr)
{
    return internet_checksum(byte_view_new(hdr, sizeof(struct ipv4_header)))
               .inner == 0;
}

/* Handle incoming packets */

struct result ipv4_handle_packet(struct input_packet *pkt,
                                 struct send_buf sb,
                                 struct arena arn)
{
    assert(pkt);

    if (pkt->data.dat == NULL ||
        pkt->data.len < (sz) sizeof(struct ipv4_header)) {
        pr_debug(
            STR("Received IPv4 datagram too small or NULL. "
                "Dropping ...\n"));
        return result_ok();
    }

    struct ipv4_header *ip_hdr = byte_buf_ptr(pkt->data);

    if (ip_hdr->version != 4) {
        pr_debug(STR("Received IPv4 datagram with version %hhu (expected 4). "
                     "Dropping ...\n"),
                 ip_hdr->version);
        return result_ok();
    }

    if (!ipv4_checksum_is_ok(ip_hdr)) {
        pr_debug(
            STR("Received IPv4 datagram with invalid checksum. "
                "Dropping ...\n"));
        return result_ok();
    }

    /* Reject packets with options (IHL != 5), no options support. */
    if ((sz) ip_hdr->ihl * 4 != sizeof(struct ipv4_header)) {
        pr_debug(STR("Received IPv4 datagram with IHL %hhu (expected %lu). "
                     "Options not supported. Dropping ...\n"),
                 ip_hdr->ihl, sizeof(struct ipv4_header) / 4);
        return result_ok();
    }

    u16 total_len = u16_from_net_u16(ip_hdr->total_length);
    if (total_len < sizeof(struct ipv4_header)) {
        pr_debug(STR("IPv4 total_length %hu is smaller than header. "
                     "Dropping ...\n"),
                 total_len);
        return result_ok();
    }

    if (total_len > pkt->data.len) {
        pr_debug(STR("IPv4 total_length %hu exceeds packet length %ld. "
                     "Dropping ...\n"),
                 total_len, pkt->data.len);
        return result_ok();
    }

    /* Drop fragments (MF flag or non-zero offset): no reassembly support.
     * This prevents fragment-based attacks (e.g., teardrop, overlapping frags).
     */
    u16 frag_offset = u16_from_net_u16(ip_hdr->fragment_offset);
    if (frag_offset & 0x3FFF) {
        pr_debug(
            STR("Received IPv4 fragment (MF or non-zero "
                "offset). "
                "Dropping ...\n"));
        return result_ok();
    }

    bool dest_ok = ipv4_addr_is_equal(ip_hdr->dest_addr, pkt->netdev->ip_addr);
#if CONFIG_NET_UDP
#if CONFIG_DHCP
    /* Also accept IPv4 broadcast for UDP (needed for DHCP server replies). */
    if (!dest_ok && ip_hdr->protocol == IPV4_PROTOCOL_UDP)
        dest_ok = ipv4_addr_is_equal(ip_hdr->dest_addr,
                                     ipv4_addr_new(255, 255, 255, 255));
#endif
#if CONFIG_NET_MDNS
    /* Accept the mDNS multicast group address (224.0.0.251, RFC 6762). */
    if (!dest_ok && ip_hdr->protocol == IPV4_PROTOCOL_UDP)
        dest_ok = ipv4_addr_is_equal(ip_hdr->dest_addr,
                                     ipv4_addr_new(224, 0, 0, 251));
#endif
#endif /* CONFIG_NET_UDP */
    if (!dest_ok) {
        pr_debug(STR("Received IPv4 datagram with destination %s (expected "
                     "%s). Dropping ...\n"),
                 ipv4_addr_format(ip_hdr->dest_addr, &arn),
                 ipv4_addr_format(pkt->netdev->ip_addr, &arn));
        return result_ok();
    }

    struct byte_view payload =
        byte_view_new(pkt->data.dat + sizeof(struct ipv4_header),
                      total_len - sizeof(struct ipv4_header));

    switch (ip_hdr->protocol) {
    case IPV4_PROTOCOL_ICMP:
        return icmpv4_handle_message(ip_hdr->src_addr, payload, sb, arn);
    case IPV4_PROTOCOL_TCP: {
        struct tcp_ip_pseudo_header pseudo_hdr = {
            .src_addr = ip_hdr->src_addr,
            .dest_addr = ip_hdr->dest_addr,
            .zero = 0,
            .protocol = ip_hdr->protocol,
            .tcp_length =
                net_u16_from_u16(total_len - sizeof(struct ipv4_header)),
        };
        return tcp_handle_packet(pseudo_hdr, payload, sb, arn);
    }
#if CONFIG_NET_UDP
    case IPV4_PROTOCOL_UDP: {
        if (payload.len < 8) {
            pr_debug(STR("UDP payload too small (%ld < 8 bytes). "
                         "Dropping ...\n"),
                     payload.len);
            return result_ok();
        }
        const u8 *udp_hdr = byte_view_ptr(payload);
        u16 src_port = (u16) (((u16) udp_hdr[0] << 8) | udp_hdr[1]);
        u16 dest_port = (u16) (((u16) udp_hdr[2] << 8) | udp_hdr[3]);
        struct byte_view udp_payload =
            byte_view_new((void *) (udp_hdr + 8), payload.len - 8);
        return udp_handle_packet(pkt->netdev, ip_hdr->src_addr, src_port,
                                 dest_port, udp_payload, sb, arn);
    }
#endif
    default:
        pr_warn(STR("Received IPv4 datagram with unknown protocol %hhu. "
                    "Dropping ...\n"),
                ip_hdr->protocol);
        return result_ok();
    }
}

/* Routing */

#define GLOBAL_ROUTE_TABLE_SIZE 32
static struct ipv4_route_entry global_route_table[GLOBAL_ROUTE_TABLE_SIZE];
static bool global_route_table_is_used[GLOBAL_ROUTE_TABLE_SIZE];

struct result ipv4_route_add(struct ipv4_route_entry ent)
{
    for (sz i = 0; i < GLOBAL_ROUTE_TABLE_SIZE; i++) {
        if (!global_route_table_is_used[i]) {
            global_route_table[i] = ent;
            global_route_table_is_used[i] = true;
            return result_ok();
        }
    }

    pr_debug(STR("IP: Route table full (max %d entries)\n"),
             GLOBAL_ROUTE_TABLE_SIZE);
    return result_error(ENOMEM);
}

static i32 count_set_bits(struct ipv4_addr addr)
{
    /* Kernighan’s algorithm for counting set bits. */

    /* The order of packing this doesn't matter. */
    u32 x = (u32) addr.addr[0] | (u32) addr.addr[1] << 8 |
            (u32) addr.addr[2] << 16 | (u32) addr.addr[3] << 24;
    i32 count = 0;

    while (x) {
        x = x & (x - 1);
        count++;
    }

    return count;
}

static struct ipv4_route_entry *ipv4_route_get_entry(struct ipv4_addr dest_ip)
{
    /* Based on TCP/IP Illustrated Volume 1, 2nd Edition, Section 5.4.2. */

    i32 best_match_n_bits = 0;
    sz best_match_index = 0;
    bool match_found = false;

    for (sz i = 0; i < GLOBAL_ROUTE_TABLE_SIZE; i++) {
        if (global_route_table_is_used[i]) {
            struct ipv4_addr masked =
                ipv4_addr_mask(dest_ip, global_route_table[i].mask);
            i32 n_bits = count_set_bits(global_route_table[i].mask);
            if (ipv4_addr_is_equal(masked, global_route_table[i].dest) &&
                n_bits >= best_match_n_bits) {
                best_match_index = i;
                best_match_n_bits = n_bits;
                match_found = true;
            }
        }
    }

    if (!match_found)
        return NULL;

    return &global_route_table[best_match_index];
}

struct result_ipv4_addr ipv4_route_interface_addr(struct ipv4_addr dest_ip)
{
    /* Temporary arena for formatting IP addresses in error messages */
    byte addr_fmt_buf[IP_ADDR_FMT_BUF_SIZE];
    struct arena addr_fmt_arn =
        arena_new(byte_array_new(addr_fmt_buf, sizeof(addr_fmt_buf)));

    struct ipv4_route_entry *route = ipv4_route_get_entry(dest_ip);
    if (!route) {
        pr_debug(STR("IP: No route for interface lookup: dest_ip=%s\n"),
                 ipv4_addr_format(dest_ip, &addr_fmt_arn));
        return result_ipv4_addr_error(EHOSTUNREACH);
    }

    return result_ipv4_addr_ok(route->interface);
}

struct result_sz ipv4_route_mtu(struct ipv4_addr dest_ip)
{
    /* Temporary arena for formatting IP addresses in error messages */
    byte addr_fmt_buf[IP_ADDR_FMT_BUF_SIZE];
    struct arena addr_fmt_arn =
        arena_new(byte_array_new(addr_fmt_buf, sizeof(addr_fmt_buf)));

    struct ipv4_route_entry *route = ipv4_route_get_entry(dest_ip);
    if (!route) {
        pr_debug(STR("IP: No route for MTU lookup: dest_ip=%s\n"),
                 ipv4_addr_format(dest_ip, &addr_fmt_arn));
        return result_sz_error(EHOSTUNREACH);
    }

    struct netdev *netdev = netdev_lookup_ip_addr(route->interface);
    if (!netdev) {
        pr_debug(STR("IP: No netdev for MTU lookup: interface=%s\n"),
                 ipv4_addr_format(route->interface, &addr_fmt_arn));
        return result_sz_error(ENODEV);
    }

    struct netdev_info ndinfo;
    netdev_get_info(netdev, &ndinfo);
    sz mtu = MAX(0, ndinfo.mtu - sizeof(struct ipv4_header));
    netdev_put(netdev);
    return result_sz_ok(mtu);
}

/* Send packets */

struct result ipv4_prepend_header(struct ipv4_addr src_ip,
                                  struct ipv4_addr dest_ip,
                                  u8 proto,
                                  u8 ttl,
                                  u16 frag_flags,
                                  struct send_buf *sb)
{
    assert(sizeof(struct ipv4_header) + send_buf_total_length(*sb) <= U16_MAX);

    /* The current total length of the send buffer is everything that will be
     * encapsulated inside the IP header constructed here. The total IP packet
     * length is the header size plus the current send buffer length.
     */

    struct ipv4_header ip_hdr = {
        .version = 4,
        .ihl = 5,
        .ds_ecn = 0,
        .total_length = net_u16_from_u16(sizeof(struct ipv4_header) +
                                         send_buf_total_length(*sb)),
        .ident = net_u16_from_u16(0),
        .fragment_offset = net_u16_from_u16(frag_flags),
        .ttl = ttl,
        .protocol = proto,
        .checksum = net_u16_from_u16(0),
        .src_addr = src_ip,
        .dest_addr = dest_ip,
    };

    ip_hdr.checksum = internet_checksum(byte_view_new(&ip_hdr, sizeof(ip_hdr)));
    assert(ipv4_checksum_is_ok(&ip_hdr));

    struct byte_buf *ip_hdr_buf = send_buf_prepend(sb, sizeof(ip_hdr));
    if (!ip_hdr_buf)
        return result_error(ENOMEM);

    /* ip_hdr_buf was allocated for sizeof(ip_hdr) bytes; always succeeds. */
    assert(
        byte_buf_append(ip_hdr_buf, byte_view_new(&ip_hdr, sizeof(ip_hdr))) ==
        sizeof(ip_hdr));

    return result_ok();
}

/* Internal: send with a pre-resolved route entry.  Avoids redundant route
 * table scan when the caller already looked up the route.
 */
static struct result ipv4_send_with_route(struct ipv4_route_entry *route,
                                          struct ipv4_addr src_ip,
                                          struct ipv4_addr dest_ip,
                                          u8 proto,
                                          struct send_buf sb,
                                          struct arena arn)
{
    struct netdev *netdev = netdev_lookup_ip_addr(route->interface);
    if (!netdev)
        return result_error(ENODEV);

    struct ipv4_addr gateway_ip = route->gateway;
    if (ipv4_addr_is_equal(route->gateway, route->interface))
        gateway_ip = dest_ip;

    struct option_mac_addr gateway_mac_opt = arp_lookup_mac_addr(gateway_ip);
    if (gateway_mac_opt.is_none) {
        send_buf_clear(&sb);
        arp_send_request(gateway_ip, netdev, sb, arn);
        netdev_put(netdev);
        return result_error(EAGAIN);
    }

#if CONFIG_PMTUD
    u16 frag_flags = (proto == IPV4_PROTOCOL_TCP) ? IPV4_FLAG_DF : 0;
#else
    u16 frag_flags = 0;
#endif
    struct result res =
        ipv4_prepend_header(src_ip, dest_ip, proto, 64, frag_flags, &sb);
    if (res.is_error) {
        netdev_put(netdev);
        return res;
    }

    struct result send_res =
        netdev_send(option_mac_addr_checked(gateway_mac_opt), netdev,
                    NETDEV_PROTO_IPV4, sb);
    netdev_put(netdev);
    return send_res;
}

struct result ipv4_send_packet_from(struct ipv4_addr src_ip,
                                    struct ipv4_addr dest_ip,
                                    u8 proto,
                                    struct send_buf sb,
                                    struct arena arn)
{
    struct ipv4_route_entry *route = ipv4_route_get_entry(dest_ip);
    if (!route) {
        pr_debug(STR("IP: No route found for dest_ip=%s\n"),
                 ipv4_addr_format(dest_ip, &arn));
        return result_error(EHOSTUNREACH);
    }

    return ipv4_send_with_route(route, src_ip, dest_ip, proto, sb, arn);
}

struct result ipv4_send_packet(struct ipv4_addr dest_ip,
                               u8 proto,
                               struct send_buf sb,
                               struct arena arn)
{
    struct result_ipv4_addr src_res = ipv4_route_interface_addr(dest_ip);
    if (src_res.is_error)
        return result_error(src_res.code);

    return ipv4_send_packet_from(result_ipv4_addr_checked(src_res), dest_ip,
                                 proto, sb, arn);
}

/* PMTU cache, protected by pmtu_lock for SMP safety. */

#if CONFIG_PMTUD
#define PMTU_CACHE_SIZE 16
#define PMTU_CACHE_AGE_MS (10ULL * 60 * 1000) /* 10 minutes, RFC 1191 §6.3 */
#define PMTU_MIN 576                          /* RFC 1191 minimum */

struct pmtu_entry {
    u32 seq;
    struct ipv4_addr dest;
    u16 pmtu;
    bool valid;
    u64 updated_ms;
};

static struct pmtu_entry pmtu_cache[PMTU_CACHE_SIZE];
static spinlock_t pmtu_lock = SPINLOCK_INITIALIZER;

static inline struct ipv4_addr ipv4_addr_from_raw(u32 raw)
{
    return ipv4_addr_new((u8) (raw >> 24), (u8) (raw >> 16), (u8) (raw >> 8),
                         (u8) raw);
}

struct pmtu_entry_snapshot {
    struct ipv4_addr dest;
    u16 pmtu;
    bool valid;
    u64 updated_ms;
};

static void pmtu_entry_write_begin(struct pmtu_entry *e)
{
    __atomic_fetch_add(&e->seq, 1, __ATOMIC_ACQ_REL);
}

static void pmtu_entry_write_end(struct pmtu_entry *e)
{
    __atomic_fetch_add(&e->seq, 1, __ATOMIC_RELEASE);
}

static struct pmtu_entry_snapshot pmtu_entry_snapshot_load(struct pmtu_entry *e)
{
    struct pmtu_entry_snapshot snap;

    for (;;) {
        u32 seq0 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
        if (seq0 & 1)
            continue;

        snap.dest = ipv4_addr_from_raw(
            ((u32) __atomic_load_n(&e->dest.addr[0], __ATOMIC_RELAXED) << 24) |
            ((u32) __atomic_load_n(&e->dest.addr[1], __ATOMIC_RELAXED) << 16) |
            ((u32) __atomic_load_n(&e->dest.addr[2], __ATOMIC_RELAXED) << 8) |
            (u32) __atomic_load_n(&e->dest.addr[3], __ATOMIC_RELAXED));
        snap.pmtu = __atomic_load_n(&e->pmtu, __ATOMIC_RELAXED);
        snap.valid = __atomic_load_n(&e->valid, __ATOMIC_RELAXED);
        snap.updated_ms = __atomic_load_n(&e->updated_ms, __ATOMIC_RELAXED);

        u32 seq1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
        if (seq0 == seq1)
            return snap;
    }
}

static void pmtu_entry_set(struct pmtu_entry *e,
                           struct ipv4_addr dest,
                           u16 pmtu,
                           u64 now)
{
    pmtu_entry_write_begin(e);
    e->dest = dest;
    e->pmtu = pmtu;
    e->valid = true;
    e->updated_ms = now;
    pmtu_entry_write_end(e);
}

u16 pmtu_update(struct ipv4_addr dest, u16 new_mtu)
{
    u64 now = time_current_ms().ms;

    if (new_mtu < PMTU_MIN)
        new_mtu = PMTU_MIN;

    u16 ret = new_mtu;
    u64 flags = spin_lock_irqsave(&pmtu_lock);

    /* Update existing entry (only decrease, per RFC 1191). */
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (pmtu_cache[i].valid &&
            ipv4_addr_is_equal(pmtu_cache[i].dest, dest)) {
            if (now - pmtu_cache[i].updated_ms >= PMTU_CACHE_AGE_MS) {
                /* Expired: treat as fresh insert (RFC 1191 §6.3). */
                pmtu_entry_set(&pmtu_cache[i], dest, new_mtu, now);
                goto out;
            }
            /* Not expired: never increase (RFC 1191 §6.1). */
            pmtu_entry_write_begin(&pmtu_cache[i]);
            if (new_mtu < pmtu_cache[i].pmtu)
                pmtu_cache[i].pmtu = new_mtu;
            pmtu_cache[i].updated_ms = now;
            ret = pmtu_cache[i].pmtu;
            pmtu_entry_write_end(&pmtu_cache[i]);
            goto out;
        }
    }

    /* Insert into first free or expired slot. */
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++) {
        if (!pmtu_cache[i].valid ||
            now - pmtu_cache[i].updated_ms >= PMTU_CACHE_AGE_MS) {
            pmtu_entry_set(&pmtu_cache[i], dest, new_mtu, now);
            goto out;
        }
    }

    /* Cache full: evict oldest entry. */
    sz oldest = 0;
    for (sz i = 1; i < PMTU_CACHE_SIZE; i++) {
        if (pmtu_cache[i].updated_ms < pmtu_cache[oldest].updated_ms)
            oldest = i;
    }
    pmtu_entry_set(&pmtu_cache[oldest], dest, new_mtu, now);

out:
    spin_unlock_irqrestore(&pmtu_lock, flags);
    return ret;
}

u16 pmtu_lookup(struct ipv4_addr dest)
{
    u64 now = time_current_ms().ms;
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++) {
        struct pmtu_entry_snapshot snap =
            pmtu_entry_snapshot_load(&pmtu_cache[i]);
        if (!snap.valid || !ipv4_addr_is_equal(snap.dest, dest))
            continue;
        if (now - snap.updated_ms >= PMTU_CACHE_AGE_MS)
            return 0;
        return snap.pmtu;
    }
    return 0;
}

#include __INC_TEST(pmtud)
#endif /* CONFIG_PMTUD */
