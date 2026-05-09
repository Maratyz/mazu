/* SPDX-License-Identifier: MIT */
/* ARP implementation for Ethernet over IPv4. */

#include <mazu/net/arp.h>
#include <mazu/net/ethernet.h>
#include <mazu/net/netdev.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>

#define ARP_ENTRY_TTL_MS (20ULL * 60 * 1000) /* 20 minutes */

/* Track last defense time for rate-limiting ARP conflict responses. */
static u64 arp_last_defend_ms;
static bool arp_has_defended;

struct arp_table_ent {
    u32 seq;
    bool is_used;
    struct ipv4_addr ip_addr;
    struct mac_addr mac_addr;
    u64 added_ms;
};

#define GLOBAL_ARP_TABLE_SIZE 32
static struct arp_table_ent global_arp_table[GLOBAL_ARP_TABLE_SIZE];
static spinlock_t arp_table_lock = SPINLOCK_INITIALIZER;

struct arp_table_ent_snapshot {
    bool is_used;
    struct ipv4_addr ip_addr;
    struct mac_addr mac_addr;
    u64 added_ms;
};

struct ip_ethernet_arp_payload {
    struct mac_addr src_mac;
    struct ipv4_addr src_ip;
    struct mac_addr dest_mac;
    struct ipv4_addr dest_ip;
} __packed;

static_assert(sizeof(struct ip_ethernet_arp_payload) == 20,
              "unexpected "
              "ip_ethernet_arp_"
              "payload size");

static void arp_table_ent_write_begin(struct arp_table_ent *ent)
{
    __atomic_fetch_add(&ent->seq, 1, __ATOMIC_ACQ_REL);
}

static void arp_table_ent_write_end(struct arp_table_ent *ent)
{
    __atomic_fetch_add(&ent->seq, 1, __ATOMIC_RELEASE);
}

static struct arp_table_ent_snapshot arp_table_ent_snapshot_load(
    struct arp_table_ent *ent)
{
    struct arp_table_ent_snapshot snap;

    for (;;) {
        u32 seq0 = __atomic_load_n(&ent->seq, __ATOMIC_ACQUIRE);
        if (seq0 & 1)
            continue;

        snap.is_used = __atomic_load_n(&ent->is_used, __ATOMIC_RELAXED);
        snap.ip_addr = ipv4_addr_new(
            __atomic_load_n(&ent->ip_addr.addr[0], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->ip_addr.addr[1], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->ip_addr.addr[2], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->ip_addr.addr[3], __ATOMIC_RELAXED));
        snap.mac_addr = mac_addr_new(
            __atomic_load_n(&ent->mac_addr.addr[0], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->mac_addr.addr[1], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->mac_addr.addr[2], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->mac_addr.addr[3], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->mac_addr.addr[4], __ATOMIC_RELAXED),
            __atomic_load_n(&ent->mac_addr.addr[5], __ATOMIC_RELAXED));
        snap.added_ms = __atomic_load_n(&ent->added_ms, __ATOMIC_RELAXED);

        u32 seq1 = __atomic_load_n(&ent->seq, __ATOMIC_ACQUIRE);
        if (seq0 == seq1)
            return snap;
    }
}

static inline bool arp_entry_is_stale(u64 now, u64 added_ms)
{
    return now - added_ms >= ARP_ENTRY_TTL_MS;
}

static struct result arp_send_common(u16 opcode,
                                     struct ipv4_addr src_ip,
                                     struct ipv4_addr dest_ip,
                                     struct mac_addr arp_target_mac,
                                     struct mac_addr eth_dest_mac,
                                     struct netdev *netdev,
                                     struct send_buf sb,
                                     struct arena tmp)
{
    assert(netdev);

    struct arp_header arp_hdr = {0};
    arp_hdr.htype = net_u16_from_u16(ARP_HTYPE_ETHERNET);
    arp_hdr.ptype = net_u16_from_u16(ETHERNET_PTYPE_IPV4);
    arp_hdr.hlen = sizeof(struct mac_addr);
    arp_hdr.plen = sizeof(struct ipv4_addr);
    arp_hdr.opcode = net_u16_from_u16(opcode);

    struct ip_ethernet_arp_payload arp_payload = {0};
    arp_payload.src_mac = netdev->mac_addr;
    arp_payload.src_ip = src_ip;
    arp_payload.dest_mac = arp_target_mac;
    arp_payload.dest_ip = dest_ip;

    struct byte_buf *buf =
        send_buf_prepend(&sb, sizeof(arp_hdr) + sizeof(arp_payload));
    if (!buf)
        return result_error(ENOMEM);

    byte_buf_append(buf, byte_view_new(&arp_hdr, sizeof(arp_hdr)));
    byte_buf_append(buf, byte_view_new(&arp_payload, sizeof(arp_payload)));

    assert(buf->len == 8 + 20);

    pr_debug(STR("Sending ARP packet (0x%hx). src_ip=%s src_mac=%s dest_ip=%s "
                 "dest_mac=%s\n"),
             opcode, ipv4_addr_format(src_ip, &tmp),
             mac_addr_format(netdev->mac_addr, &tmp),
             ipv4_addr_format(dest_ip, &tmp),
             mac_addr_format(arp_target_mac, &tmp));

    return netdev_send(eth_dest_mac, netdev, NETDEV_PROTO_ARP, sb);
}

struct result arp_send_request(struct ipv4_addr dest_ip,
                               struct netdev *netdev,
                               struct send_buf sb,
                               struct arena tmp)
{
    assert(netdev);
    return arp_send_common(ARP_OPCODE_REQUEST, netdev->ip_addr, dest_ip,
                           MAC_ADDR_BROADCAST, MAC_ADDR_BROADCAST, netdev, sb,
                           tmp);
}

struct option_mac_addr arp_lookup_mac_addr(struct ipv4_addr ip_addr)
{
    u64 now = time_current_ms().ms;

    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++) {
        struct arp_table_ent_snapshot snap =
            arp_table_ent_snapshot_load(&global_arp_table[i]);
        if (!snap.is_used)
            continue;
        if (arp_entry_is_stale(now, snap.added_ms))
            continue;
        if (ipv4_addr_is_equal(snap.ip_addr, ip_addr))
            return option_mac_addr_ok(snap.mac_addr);
    }

    return option_mac_addr_none();
}

static void arp_table_ent_clear(struct arp_table_ent *ent)
{
    arp_table_ent_write_begin(ent);
    ent->is_used = false;
    arp_table_ent_write_end(ent);
}

static void arp_table_ent_store(struct arp_table_ent *ent,
                                struct ipv4_addr ip_addr,
                                struct mac_addr mac_addr,
                                u64 now)
{
    arp_table_ent_write_begin(ent);
    ent->ip_addr = ip_addr;
    ent->mac_addr = mac_addr;
    ent->added_ms = now;
    ent->is_used = true;
    arp_table_ent_write_end(ent);
}

static struct result_bool arp_table_update_or_insert(struct ipv4_addr ip_addr,
                                                     struct mac_addr mac_addr)
{
    u64 now = time_current_ms().ms;

    u64 flags = spin_lock_irqsave(&arp_table_lock);

    /* If the given IPv4 address already has an entry in the table, update its
     * MAC address and refresh timestamp.
     */
    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++) {
        if (global_arp_table[i].is_used &&
            ipv4_addr_is_equal(ip_addr, global_arp_table[i].ip_addr)) {
            arp_table_ent_store(&global_arp_table[i], ip_addr, mac_addr, now);
            spin_unlock_irqrestore(&arp_table_lock, flags);
            return result_bool_ok(true);
        }
    }

    /* No existing entry; create a new one, reclaiming stale entries. */
    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++) {
        if (!global_arp_table[i].is_used ||
            arp_entry_is_stale(now, global_arp_table[i].added_ms)) {
            arp_table_ent_store(&global_arp_table[i], ip_addr, mac_addr, now);
            spin_unlock_irqrestore(&arp_table_lock, flags);
            return result_bool_ok(false);
        }
    }

    spin_unlock_irqrestore(&arp_table_lock, flags);
    return result_bool_error(ENOMEM);
}

void arp_for_each(arp_iter_cb_t cb, void *ctx)
{
    struct arp_entry_info snap[GLOBAL_ARP_TABLE_SIZE];
    sz n = 0;
    u64 now = time_current_ms().ms;

    u64 flags = spin_lock_irqsave(&arp_table_lock);
    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++) {
        if (!global_arp_table[i].is_used)
            continue;
        if (arp_entry_is_stale(now, global_arp_table[i].added_ms)) {
            arp_table_ent_clear(&global_arp_table[i]);
            continue;
        }
        snap[n++] = (struct arp_entry_info) {
            .ip_addr = global_arp_table[i].ip_addr,
            .mac_addr = global_arp_table[i].mac_addr,
            .age_ms = now - global_arp_table[i].added_ms,
        };
    }
    spin_unlock_irqrestore(&arp_table_lock, flags);

    for (sz i = 0; i < n; i++)
        cb(snap[i], ctx);
}

struct result arp_handle_packet(struct input_packet *pkt,
                                struct send_buf sb,
                                struct arena tmp)
{
    if (pkt->data.len <
        sizeof(struct arp_header) + sizeof(struct ip_ethernet_arp_payload)) {
        pr_debug(
            STR("Received ARP packet smaller than ARP header "
                "with "
                "IPv4 over Ethernet payload. Dropping ...\n"));
        return result_ok();
    }

    struct arp_header *arp_hdr = byte_buf_ptr(pkt->data);

    if (u16_from_net_u16(arp_hdr->htype) != ARP_HTYPE_ETHERNET ||
        u16_from_net_u16(arp_hdr->ptype) != ETHERNET_PTYPE_IPV4) {
        pr_debug(STR("Received ARP packet with unknown htype=0x%hx or "
                     "ptype=0x%hx. Dropping ...\n"),
                 u16_from_net_u16(arp_hdr->htype),
                 u16_from_net_u16(arp_hdr->ptype));
        return result_ok();
    }

    if (arp_hdr->hlen != sizeof(struct mac_addr) ||
        arp_hdr->plen != sizeof(struct ipv4_addr)) {
        pr_warn(STR("Received ARP packet with hlen=%hhu and plen=%hhu. These "
                    "are wrong for IPv4 over Ethernet. Continuing assuming "
                    "hlen=6 and plen=4\n"),
                arp_hdr->hlen, arp_hdr->plen);
    }

    struct ip_ethernet_arp_payload *payload =
        (struct ip_ethernet_arp_payload *) (pkt->data.dat +
                                            sizeof(struct arp_header));

    struct option_mac_addr old_mac_opt = arp_lookup_mac_addr(payload->src_ip);

    bool is_probe = ipv4_addr_is_equal(payload->src_ip, IPV4_ADDR_ANY);
    bool is_conflict =
        ipv4_addr_is_equal(payload->src_ip, pkt->netdev->ip_addr) &&
        !mac_addr_is_equal(payload->src_mac, pkt->netdev->mac_addr);

    struct result_bool insert_res = result_bool_ok(false);
    if (!is_probe && !is_conflict) {
        insert_res =
            arp_table_update_or_insert(payload->src_ip, payload->src_mac);
        if (insert_res.is_error) {
            pr_warn(STR("Failed to update ARP table: 0x%hx\n"),
                    insert_res.code);
            return result_error(insert_res.code);
        }
    }

    pr_debug(STR("Received ARP packet and updated ARP table with ip_addr=%s "
                 "mac_addr=%s (old mac_addr=%s)\n"),
             ipv4_addr_format(payload->src_ip, &tmp),
             mac_addr_format(payload->src_mac, &tmp),
             (!is_probe && !is_conflict && result_bool_checked(insert_res))
                 ? mac_addr_format(option_mac_addr_checked(old_mac_opt), &tmp)
                 : STR("none"));

    /* ARP defend: if someone else claims the host IP, send a defense reply
     * (rate-limited to once per ARP_DEFEND_INTERVAL_MS).
     * Defend state is protected by arp_table_lock for SMP safety.
     */
    if (is_conflict) {
        u64 now = time_current_ms().ms;
        bool should_defend = false;

        u64 flags = spin_lock_irqsave(&arp_table_lock);
        if (!arp_has_defended ||
            now - arp_last_defend_ms >= ARP_DEFEND_INTERVAL_MS) {
            arp_has_defended = true;
            arp_last_defend_ms = now;
            should_defend = true;
        }
        spin_unlock_irqrestore(&arp_table_lock, flags);

        if (should_defend) {
            pr_warn(STR("ARP conflict: %s claims the host IP %s. Sending "
                        "defense.\n"),
                    mac_addr_format(payload->src_mac, &tmp),
                    ipv4_addr_format(payload->src_ip, &tmp));
            return arp_send_common(ARP_OPCODE_REPLY, pkt->netdev->ip_addr,
                                   payload->src_ip, MAC_ADDR_BROADCAST,
                                   MAC_ADDR_BROADCAST, pkt->netdev, sb, tmp);
        }
        pr_warn(STR("ARP conflict: %s claims the host IP %s. Defense "
                    "rate-limited.\n"),
                mac_addr_format(payload->src_mac, &tmp),
                ipv4_addr_format(payload->src_ip, &tmp));
        return result_ok();
    }

    /* Only reply if the request targets the device IP address. */
    if (u16_from_net_u16(arp_hdr->opcode) == ARP_OPCODE_REQUEST) {
        if (!ipv4_addr_is_equal(payload->dest_ip, pkt->netdev->ip_addr))
            return result_ok();
        return arp_send_common(ARP_OPCODE_REPLY, pkt->netdev->ip_addr,
                               payload->src_ip, payload->src_mac,
                               payload->src_mac, pkt->netdev, sb, tmp);
    }
    return result_ok();
}

struct result arp_send_gratuitous(struct netdev *netdev,
                                  struct send_buf sb,
                                  struct arena tmp)
{
    assert(netdev);
    return arp_send_common(ARP_OPCODE_REPLY, netdev->ip_addr, netdev->ip_addr,
                           MAC_ADDR_BROADCAST, MAC_ADDR_BROADCAST, netdev, sb,
                           tmp);
}

#include __INC_TEST(arp)
