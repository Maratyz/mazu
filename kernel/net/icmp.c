/* SPDX-License-Identifier: MIT */
#include <mazu/byte.h>
#include <mazu/callout.h>
#include <mazu/kvalloc.h>
#include <mazu/net/icmp.h>
#include <mazu/net/ip.h>
#include <mazu/net/tcp.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/string.h>
#include <mazu/time.h>
#include "../sched/waitqueue.h"

#define ICMPV4_TYPE_ECHO_REPLY 0
#define ICMPV4_TYPE_DEST_UNREACH 3
#define ICMPV4_TYPE_ECHO 8

#define ICMPV4_CODE_FRAG_NEEDED 4

struct icmpv4_header {
    u8 type;
    u8 code;
    net_u16 checksum;
};

struct icmpv4_echo_message {
    net_u16 ident;
    net_u16 seq;
};

static inline bool icmpv4_checksum_is_ok(struct byte_view message)
{
    return internet_checksum(message).inner == 0;
}

/* Ping table - RTT tracking for outstanding echo requests */

#define PING_TABLE_SIZE 4

struct ping_entry {
    u64 send_ticks;
    u64 rtt_us;
    u16 ident;
    u16 seq;
    bool valid;
    bool replied;
};

static struct ping_entry ping_table[PING_TABLE_SIZE];
static spinlock_t ping_lock = SPINLOCK_INITIALIZER;
static struct wait_queue_head ping_waitq =
    WAIT_QUEUE_HEAD_INITIALIZER(ping_waitq);

/* Record an outgoing echo request in the ping table (upsert). */
static void ping_entry_set(struct ping_entry *e,
                           u16 ident,
                           u16 seq,
                           u64 send_ticks)
{
    e->ident = ident;
    e->seq = seq;
    e->send_ticks = send_ticks;
    e->valid = true;
    e->replied = false;
    e->rtt_us = 0;
}

static void ping_table_record(u16 ident, u16 seq, u64 send_ticks)
{
    u64 flags = spin_lock_irqsave(&ping_lock);

    /* Upsert: if a matching entry already exists (e.g., from a retry after
     * send failure), update it in place rather than creating a duplicate.
     */
    for (sz i = 0; i < PING_TABLE_SIZE; i++) {
        if (ping_table[i].valid && ping_table[i].ident == ident &&
            ping_table[i].seq == seq) {
            ping_entry_set(&ping_table[i], ident, seq, send_ticks);
            spin_unlock_irqrestore(&ping_lock, flags);
            return;
        }
    }

    /* Insert into first free slot. */
    for (sz i = 0; i < PING_TABLE_SIZE; i++) {
        if (!ping_table[i].valid) {
            ping_entry_set(&ping_table[i], ident, seq, send_ticks);
            spin_unlock_irqrestore(&ping_lock, flags);
            return;
        }
    }
    /* Table full: overwrite the oldest entry (slot 0) and shift. */
    for (sz i = 0; i < PING_TABLE_SIZE - 1; i++)
        ping_table[i] = ping_table[i + 1];
    ping_entry_set(&ping_table[PING_TABLE_SIZE - 1], ident, seq, send_ticks);

    spin_unlock_irqrestore(&ping_lock, flags);
}

/* Look up an echo reply and record RTT.  Returns true if found. */
static bool ping_table_complete(u16 ident, u16 seq, u64 recv_ticks)
{
    u64 flags = spin_lock_irqsave(&ping_lock);

    for (sz i = 0; i < PING_TABLE_SIZE; i++) {
        if (ping_table[i].valid && ping_table[i].ident == ident &&
            ping_table[i].seq == seq && !ping_table[i].replied) {
            ping_table[i].replied = true;
            ping_table[i].rtt_us =
                time_ticks_to_us(recv_ticks - ping_table[i].send_ticks);
            spin_unlock_irqrestore(&ping_lock, flags);
            return true;
        }
    }

    spin_unlock_irqrestore(&ping_lock, flags);
    return false;
}

/* Check if a specific (ident, seq) has been replied to. */
static bool ping_table_is_replied(u16 ident, u16 seq, u64 *rtt_us)
{
    u64 flags = spin_lock_irqsave(&ping_lock);

    for (sz i = 0; i < PING_TABLE_SIZE; i++) {
        if (ping_table[i].valid && ping_table[i].ident == ident &&
            ping_table[i].seq == seq) {
            if (rtt_us)
                *rtt_us = ping_table[i].rtt_us;
            bool replied = ping_table[i].replied;
            spin_unlock_irqrestore(&ping_lock, flags);
            return replied;
        }
    }

    spin_unlock_irqrestore(&ping_lock, flags);
    return false;
}

/* Clear a specific entry. */
static void ping_table_clear(u16 ident, u16 seq)
{
    u64 flags = spin_lock_irqsave(&ping_lock);

    for (sz i = 0; i < PING_TABLE_SIZE; i++) {
        if (ping_table[i].valid && ping_table[i].ident == ident &&
            ping_table[i].seq == seq) {
            ping_table[i].valid = false;
            spin_unlock_irqrestore(&ping_lock, flags);
            return;
        }
    }

    spin_unlock_irqrestore(&ping_lock, flags);
}

/* Send echo */

struct result icmpv4_send_echo(struct ipv4_addr dest_addr,
                               u16 ident,
                               u16 seq,
                               struct send_buf sb,
                               struct arena arn)
{
    struct icmpv4_header icmp_hdr = {0};
    icmp_hdr.type = ICMPV4_TYPE_ECHO;
    icmp_hdr.code = 0;
    icmp_hdr.checksum = net_u16_from_u16(0);

    struct icmpv4_echo_message icmp_echo = {0};
    icmp_echo.ident = net_u16_from_u16(ident);
    icmp_echo.seq = net_u16_from_u16(seq);

    sz n_data_bytes = 40;
    struct byte_buf *reply_buf = send_buf_prepend(
        &sb, sizeof(icmp_hdr) + sizeof(icmp_echo) + n_data_bytes);
    if (!reply_buf)
        return result_error(ENOMEM);

    assert(byte_buf_append(reply_buf,
                           byte_view_new(&icmp_hdr, sizeof(icmp_hdr))) ==
           sizeof(icmp_hdr));
    assert(byte_buf_append(reply_buf,
                           byte_view_new(&icmp_echo, sizeof(icmp_echo))) ==
           sizeof(icmp_echo));
    assert(byte_buf_append_n(reply_buf, n_data_bytes, 0xb0) == n_data_bytes);

    /* Patch-in the checksum at the right place. */
    net_u16 *dat = byte_buf_ptr(*reply_buf);
    dat[1] = internet_checksum(byte_view_from_buf(*reply_buf));

    /* Record in ping table before sending. */
    ping_table_record(ident, seq, time_rdtime());

    pr_debug(STR("Sending ICMPv4 echo message to dest_addr=%s ident=0x%hx "
                 "seq=0x%hx\n"),
             ipv4_addr_format(dest_addr, &arn), ident, seq);

    return ipv4_send_packet(dest_addr, IPV4_PROTOCOL_ICMP, sb, arn);
}

/* Handle incoming messages */

static struct result icmpv4_handle_echo(struct ipv4_addr dest_addr,
                                        struct icmpv4_header *hdr,
                                        struct byte_view data,
                                        struct send_buf sb,
                                        struct arena arn)
{
    if (data.len < sizeof(struct icmpv4_echo_message)) {
        pr_debug(
            STR("Received ICMPv4 echo message with length too "
                "short to fit the identifier and sequence "
                "number. "
                "Dropping ...\n"));
        return result_ok();
    }

    if (hdr->code != 0) {
        pr_debug(
            STR("Received ICMPv4 echo message with non-zero "
                "code. "
                "Dropping ...\n"));
        return result_ok();
    }

    struct icmpv4_echo_message *icmp_echo = byte_view_ptr(data);
    pr_debug(STR("Received ICMPv4 echo message from %s ident=%hx seq=%hx\n"),
             ipv4_addr_format(dest_addr, &arn),
             u16_from_net_u16(icmp_echo->ident),
             u16_from_net_u16(icmp_echo->seq));

    struct icmpv4_header icmp_hdr = {0};
    icmp_hdr.type = ICMPV4_TYPE_ECHO_REPLY;
    icmp_hdr.code = 0;
    icmp_hdr.checksum = net_u16_from_u16(0);

    struct byte_buf *reply_buf =
        send_buf_prepend(&sb, sizeof(icmp_hdr) + data.len);
    if (!reply_buf)
        return result_error(ENOMEM);

    assert(byte_buf_append(reply_buf,
                           byte_view_new(&icmp_hdr, sizeof(icmp_hdr))) ==
           sizeof(icmp_hdr));
    assert(byte_buf_append(reply_buf, data) == data.len);

    /* Patch-in the checksum at the right place. */
    net_u16 *dat = byte_buf_ptr(*reply_buf);
    dat[1] = internet_checksum(byte_view_from_buf(*reply_buf));

    pr_debug(STR("Sending ICMPv4 echo reply message to dest_addr=%s\n"),
             ipv4_addr_format(dest_addr, &arn));

    return ipv4_send_packet(dest_addr, IPV4_PROTOCOL_ICMP, sb, arn);
}

static struct result icmpv4_handle_echo_reply(struct ipv4_addr src_addr,
                                              struct icmpv4_header *hdr,
                                              struct byte_view data,
                                              struct arena arn)
{
    if (data.len < sizeof(struct icmpv4_echo_message)) {
        pr_debug(
            STR("Received ICMPv4 echo reply message with length "
                "too short to fit the identifier and sequence "
                "number. Dropping ...\n"));
        return result_ok();
    }

    if (hdr->code != 0) {
        pr_debug(
            STR("Received ICMPv4 echo reply message with "
                "non-zero "
                "code. Dropping ...\n"));
        return result_ok();
    }

    struct icmpv4_echo_message *icmp_echo = byte_view_ptr(data);
    u16 ident = u16_from_net_u16(icmp_echo->ident);
    u16 seq = u16_from_net_u16(icmp_echo->seq);

    /* Record RTT in the ping table. */
    u64 now = time_rdtime();
    bool found = ping_table_complete(ident, seq, now);

    if (found) {
        u64 rtt_us = 0;
        ping_table_is_replied(ident, seq, &rtt_us);
        wake_up(&ping_waitq, 0);
        pr_debug(STR("Received ICMPv4 echo reply from %s ident=0x%hx "
                     "seq=0x%hx rtt=%lu us\n"),
                 ipv4_addr_format(src_addr, &arn), ident, seq, rtt_us);
    } else {
        pr_debug(STR("Received ICMPv4 echo reply from %s ident=0x%hx "
                     "seq=0x%hx (unsolicited)\n"),
                 ipv4_addr_format(src_addr, &arn), ident, seq);
    }

    return result_ok();
}

/* ICMP Destination Unreachable - Fragmentation Needed (PMTUD) */

#if CONFIG_PMTUD
struct icmpv4_dest_unreach_header {
    net_u16 unused;
    net_u16 next_hop_mtu;
} __packed;

#define IPV4_HEADER_MIN_SIZE 20

static struct result icmpv4_handle_dest_unreach(struct ipv4_addr src_addr,
                                                struct icmpv4_header *hdr,
                                                struct byte_view data,
                                                struct arena arn)
{
    if (hdr->code != ICMPV4_CODE_FRAG_NEEDED)
        return result_ok();

    /* Data must contain the unused/next_hop_mtu fields (4 bytes) plus at
     * least the original IP header (20 bytes) to extract the
     * original destination address.
     */
    if (data.len <
        (sz) (sizeof(struct icmpv4_dest_unreach_header) + IPV4_HEADER_MIN_SIZE))
        return result_ok();

    struct icmpv4_dest_unreach_header *unreach_hdr = byte_view_ptr(data);
    u16 next_hop_mtu = u16_from_net_u16(unreach_hdr->next_hop_mtu);

    /* Extract the original destination from the embedded IP header.
     * dest_addr sits at offset 16 in the standard 20-byte IPv4 header.
     */
    struct byte_view orig_ip_data =
        byte_view_skip(data, sizeof(struct icmpv4_dest_unreach_header));
    struct ipv4_addr orig_dest;
    memcpy(&orig_dest, orig_ip_data.dat + 16, sizeof(orig_dest));

    if (next_hop_mtu == 0) {
        /* RFC 1191 §5: older routers may send 0; fall back to minimum. */
        next_hop_mtu = 576;
    }

    pr_debug(STR("ICMP Frag Needed from %s: next_hop_mtu=%hu for dest=%s\n"),
             ipv4_addr_format(src_addr, &arn), next_hop_mtu,
             ipv4_addr_format(orig_dest, &arn));

    u16 clamped_mtu = pmtu_update(orig_dest, next_hop_mtu);
    tcp_pmtu_update(orig_dest, clamped_mtu);

    return result_ok();
}
#endif /* CONFIG_PMTUD */

struct result icmpv4_handle_message(struct ipv4_addr src_addr,
                                    struct byte_view message,
                                    struct send_buf sb,
                                    struct arena arn)
{
    if (message.len < sizeof(struct icmpv4_header)) {
        pr_debug(
            STR("Received ICMPv4 message smaller than the "
                "ICMPv4 "
                "header. Dropping ...\n"));
        return result_ok();
    }

    struct icmpv4_header *icmp_hdr = byte_view_ptr(message);

    if (!icmpv4_checksum_is_ok(message)) {
        pr_debug(
            STR("Received ICMPv4 message with invalid checksum. "
                "Dropping ...\n"));
        return result_ok();
    }

    struct byte_view data =
        byte_view_skip(message, sizeof(struct icmpv4_header));

    switch (icmp_hdr->type) {
    case ICMPV4_TYPE_ECHO:
        return icmpv4_handle_echo(src_addr, icmp_hdr, data, sb, arn);
    case ICMPV4_TYPE_ECHO_REPLY:
        return icmpv4_handle_echo_reply(src_addr, icmp_hdr, data, arn);
#if CONFIG_PMTUD
    case ICMPV4_TYPE_DEST_UNREACH:
        return icmpv4_handle_dest_unreach(src_addr, icmp_hdr, data, arn);
#endif
    default:
        pr_debug(STR("Received ICMPv4 message with unknown type 0x%hhx. "
                     "Dropping ...\n"),
                 icmp_hdr->type);
        return result_ok();
    }
}

/* Blocking ping with RTT statistics. */

struct ping_stats icmp_ping(struct ipv4_addr dest, u16 ident, u16 count)
{
    DEBUG_ASSERT(!in_interrupt_context());
    DEBUG_ASSERT(sched_current_task() != NULL);

    struct ping_stats stats = {0, 0, U64_MAX, 0, 0};

    struct option_byte_array sb_opt = kvalloc_alloc(0x4000, 64);
    struct option_byte_array tmp_opt = kvalloc_alloc(0x2000, 64);
    if (sb_opt.is_none || tmp_opt.is_none) {
        if (!sb_opt.is_none)
            kvalloc_free(sb_opt.unchecked_option_value);
        if (!tmp_opt.is_none)
            kvalloc_free(tmp_opt.unchecked_option_value);
        return stats;
    }
    struct byte_array sb_ba = option_byte_array_checked(sb_opt);
    struct byte_array tmp_ba = option_byte_array_checked(tmp_opt);

    u64 rtt_sum = 0;

    for (u16 seq = 0; seq < count; seq++) {
        struct send_buf sb = send_buf_new(arena_new(sb_ba));
        struct arena tmp = arena_new(tmp_ba);

        struct result res = icmpv4_send_echo(dest, ident, seq, sb, tmp);
        if (res.is_error && res.code == EAGAIN) {
            /* ARP not ready; retry once after a short delay. */
            sleep_ms(time_ms_new(500));
            sb = send_buf_new(arena_new(sb_ba));
            tmp = arena_new(tmp_ba);
            res = icmpv4_send_echo(dest, ident, seq, sb, tmp);
        }

        if (res.is_error) {
            stats.sent++;
            /* Clean up the stale ping table entry from the failed send. */
            ping_table_clear(ident, seq);
            sleep_ms(time_ms_new(1000));
            continue;
        }

        stats.sent++;

        /* Wait up to 2s for this specific reply. */
        u64 rtt_us = 0;
        bool got_reply = ping_table_is_replied(ident, seq, &rtt_us);
        if (!got_reply) {
            struct wait_queue_entry wqe = {
                .task = sched_current_task(),
                .reason = WAIT_UNBLOCK_NONE,
            };
            list_init(&wqe.node);

            struct callout tmo;
            callout_init(&tmo);
            wqe.timeout_callout = &tmo;
            struct wait_timeout_arg tmo_arg = {
                .wq = &ping_waitq,
                .wqe = &wqe,
            };

            prepare_to_wait(&ping_waitq, &wqe);
            callout_set_ticks(&tmo, time_ms_to_ticks(2000), wait_timeout_fn,
                              &tmo_arg);

            for (;;) {
                got_reply = ping_table_is_replied(ident, seq, &rtt_us);
                if (got_reply || wait_unblock_is_terminal(wqe.reason))
                    break;
                sched_yield_trap();
                got_reply = ping_table_is_replied(ident, seq, &rtt_us);
                if (got_reply || wait_unblock_is_terminal(wqe.reason))
                    break;
                /* WAKEUP on a shared queue may be from another ping's reply.
                 * Reset the reason and re-check our specific predicate.
                 */
                wqe.reason = WAIT_UNBLOCK_NONE;
                prepare_to_wait(&ping_waitq, &wqe);
            }

            callout_cancel_sync(&tmo);
            finish_wait(&ping_waitq, &wqe);
        }

        if (got_reply) {
            stats.received++;
            rtt_sum += rtt_us;
            if (rtt_us < stats.min_rtt_us)
                stats.min_rtt_us = rtt_us;
            if (rtt_us > stats.max_rtt_us)
                stats.max_rtt_us = rtt_us;
        }

        ping_table_clear(ident, seq);

        /* Inter-ping spacing: 1s total from send, minus time already spent. */
        if (seq + 1 < count)
            sleep_ms(time_ms_new(500));
    }

    if (stats.received > 0)
        stats.avg_rtt_us = rtt_sum / stats.received;
    if (stats.min_rtt_us == U64_MAX)
        stats.min_rtt_us = 0;

    kvalloc_free(tmp_ba);
    kvalloc_free(sb_ba);

    return stats;
}

/* Self-tests */

#include __INC_TEST(icmp)
