/* SPDX-License-Identifier: MIT */
/* DNS stub resolver - synchronous A-record queries over UDP.
 *
 * The resolver picks an ephemeral port, registers a temporary UDP listener,
 * builds an A-record query, sends it to the nameserver, and then blocks on a
 * wait queue until a reply or timeout arrives. Pure query/parse functions are
 * public and independently testable.
 */

#include <mazu/asm.h>
#include <mazu/byte.h>
#include <mazu/net/dns.h>
#include <mazu/net/ip.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/udp.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>

#include "../sched/waitqueue.h"
#include "dns_wire.h"

/* DNS constants (DNS_FLAG_QR, DNS_QTYPE_A, DNS_QCLASS_IN in dns_wire.h) */
#define DNS_PORT 53
#define DNS_FLAG_RD 0x0100u /* recursion desired */
#define DNS_RCODE_MASK 0x000Fu
#define DNS_RCODE_NXDOMAIN 3
#define DNS_MAX_PACKET 512

/* dns_build_query */

sz dns_build_query(struct str hostname, u16 tx_id, u8 *out, sz out_cap)
{
    /* Worst case: 12 (header) + hostname.len + 2 (leading len + trailing 0)
     * + 4 (QTYPE + QCLASS).  Hostname labels add one length byte per label
     * but the dot between labels is consumed, so net overhead is +1 for the
     * first label length byte.
     */
    if (hostname.len == 0 || hostname.len > 253)
        return 0;

    sz max_needed = 12 + hostname.len + 2 + 4;
    if (out_cap < max_needed)
        return 0;

    sz w = 0;

    /* Header: ID, flags=RD, QDCOUNT=1, AN/NS/AR=0 */
    dns_write_u16(out + w, tx_id);
    w += 2;
    dns_write_u16(out + w, DNS_FLAG_RD);
    w += 2;
    dns_write_u16(out + w, 1);
    w += 2; /* QDCOUNT */
    dns_write_u16(out + w, 0);
    w += 2; /* ANCOUNT */
    dns_write_u16(out + w, 0);
    w += 2; /* NSCOUNT */
    dns_write_u16(out + w, 0);
    w += 2; /* ARCOUNT */

    /* QNAME: convert "example.com" to wire labels (7 example 3 com 0). */
    sz label_start = w;
    w++; /* reserve space for first label length */

    for (sz i = 0; i < hostname.len; i++) {
        if (hostname.dat[i] == '.') {
            sz label_len = w - label_start - 1;
            if (label_len == 0 || label_len > 63)
                return 0;
            out[label_start] = (u8) label_len;
            label_start = w;
            w++; /* reserve next label length */
        } else {
            out[w++] = (u8) hostname.dat[i];
        }
    }
    /* Final label */
    sz label_len = w - label_start - 1;
    if (label_len == 0 || label_len > 63)
        return 0;
    out[label_start] = (u8) label_len;
    out[w++] = 0; /* root label */

    /* QTYPE=A, QCLASS=IN */
    dns_write_u16(out + w, DNS_QTYPE_A);
    w += 2;
    dns_write_u16(out + w, DNS_QCLASS_IN);
    w += 2;

    return w;
}

/* dns_parse_response

 * Skip a DNS name (handles compression pointers).  Returns bytes consumed
 * from the current position, or 0 on error.
 */
static sz dns_skip_name(const u8 *pkt, sz pkt_len, sz off)
{
    sz orig = off;
    bool jumped = false;
    sz jump_return = 0;
    i32 max_jumps = 16; /* prevent infinite loops from cyclic pointers */

    while (off < pkt_len) {
        u8 b = pkt[off];
        if (b == 0) {
            if (!jumped)
                return off - orig + 1;
            return jump_return - orig;
        }
        if ((b & 0xC0) == 0xC0) {
            /* Compression pointer */
            if (off + 1 >= pkt_len)
                return 0;
            if (--max_jumps <= 0)
                return 0; /* cycle detected */
            if (!jumped)
                jump_return = off + 2;
            jumped = true;
            off = (sz) (((b & 0x3F) << 8) | pkt[off + 1]);
            continue;
        }
        if (b & 0xC0)
            return 0; /* reserved label type */
        off += 1 + b;
    }
    return 0; /* truncated */
}

struct result dns_parse_response(const u8 *data,
                                 sz len,
                                 u16 expected_tx_id,
                                 struct ipv4_addr *out_addr)
{
    if (len < 12)
        return result_error(EINVAL);

    u16 tx_id = dns_read_u16(data);
    if (tx_id != expected_tx_id)
        return result_error(EINVAL);

    u16 flags = dns_read_u16(data + 2);
    if (!(flags & DNS_FLAG_QR))
        return result_error(EINVAL); /* not a response */

    u16 rcode = flags & DNS_RCODE_MASK;
    if (rcode == DNS_RCODE_NXDOMAIN)
        return result_error(ENOENT);
    if (rcode != 0)
        return result_error(EINVAL);

    u16 qdcount = dns_read_u16(data + 4);
    u16 ancount = dns_read_u16(data + 6);

    /* Skip question section */
    sz off = 12;
    for (u16 q = 0; q < qdcount; q++) {
        sz n = dns_skip_name(data, len, off);
        if (n == 0)
            return result_error(EINVAL);
        off += n;
        off += 4; /* QTYPE + QCLASS */
        if (off > len)
            return result_error(EINVAL);
    }

    /* Walk answer section, find first A record */
    for (u16 a = 0; a < ancount; a++) {
        sz n = dns_skip_name(data, len, off);
        if (n == 0)
            return result_error(EINVAL);
        off += n;

        if (off + 10 > len)
            return result_error(EINVAL);

        u16 rtype = dns_read_u16(data + off);
        u16 rclass = dns_read_u16(data + off + 2);
        /* skip TTL (4 bytes) */
        u16 rdlength = dns_read_u16(data + off + 8);
        off += 10;

        if (off + rdlength > len)
            return result_error(EINVAL);

        if (rtype == DNS_QTYPE_A && (rclass & 0x7FFF) == DNS_QCLASS_IN &&
            rdlength == 4) {
            *out_addr = ipv4_addr_new(data[off], data[off + 1], data[off + 2],
                                      data[off + 3]);
            return result_ok();
        }

        off += rdlength;
    }

    return result_error(ENOENT); /* no A record found */
}

/* dns_resolve (blocking)

 * Shared state between dns_resolve() and its UDP callback.
 * dns_in_flight prevents concurrent resolves (checked under dns_lock).
 * dns_lock protects the response buffer for the produce/consume handoff.
 */
static bool dns_response_ready;
static u8 dns_response_buf[DNS_MAX_PACKET];
static sz dns_response_len;
static spinlock_t dns_lock = SPINLOCK_INITIALIZER;
static bool dns_in_flight;
static struct ipv4_addr dns_expected_ns;
static struct wait_queue_head dns_waitq =
    WAIT_QUEUE_HEAD_INITIALIZER(dns_waitq);

static inline bool dns_response_is_ready(void)
{
    return __atomic_load_n(&dns_response_ready, __ATOMIC_ACQUIRE);
}

static void dns_set_in_flight(bool in_flight)
{
    u64 flags = spin_lock_irqsave(&dns_lock);
    dns_in_flight = in_flight;
    spin_unlock_irqrestore(&dns_lock, flags);
}

static struct result dns_udp_handler(struct netdev *recv_dev __unused,
                                     struct ipv4_addr src_ip,
                                     u16 src_port,
                                     u16 dest_port __unused,
                                     struct byte_view payload,
                                     struct send_buf sb __unused,
                                     struct arena tmp __unused)
{
    /* Only accept replies from the expected nameserver on port 53. */
    if (src_port != DNS_PORT)
        return result_ok();

    sz copy_len = payload.len;
    if (copy_len > DNS_MAX_PACKET)
        copy_len = DNS_MAX_PACKET;

    const u8 *src = byte_view_ptr(payload);

    u64 flags = spin_lock_irqsave(&dns_lock);

    /* Check expected nameserver under lock - dns_resolve writes it under the
     * same lock, so this guarantees the correct value is visible value on
     * RVWMO.
     */
    if (!ipv4_addr_is_equal(src_ip, dns_expected_ns)) {
        spin_unlock_irqrestore(&dns_lock, flags);
        return result_ok();
    }

    for (sz i = 0; i < copy_len; i++)
        dns_response_buf[i] = src[i];
    dns_response_len = copy_len;
    __atomic_store_n(&dns_response_ready, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&dns_lock, flags);
    wake_up(&dns_waitq, 1);

    return result_ok();
}

struct result_ipv4_addr dns_resolve(struct ipv4_addr nameserver,
                                    struct str hostname,
                                    struct send_buf sb,
                                    struct arena arn)
{
    DEBUG_ASSERT(!in_interrupt_context());
    DEBUG_ASSERT(sched_current_task() != NULL);

    /* Only one resolve in flight at a time (shared response buffer). */
    u64 gflags = spin_lock_irqsave(&dns_lock);
    if (dns_in_flight) {
        spin_unlock_irqrestore(&dns_lock, gflags);
        return result_ipv4_addr_error(EBUSY);
    }
    dns_in_flight = true;
    dns_expected_ns = nameserver;
    __atomic_store_n(&dns_response_ready, false, __ATOMIC_RELEASE);
    dns_response_len = 0;
    spin_unlock_irqrestore(&dns_lock, gflags);

    /* Pick an ephemeral source port and transaction ID using PRNG to
     * resist DNS cache poisoning (wider port range + less predictable ID).
     */
    u64 rand_val;
    pseudo_rand_u64(&rand_val);
    u16 ephemeral_port = (u16) (49152 + (rand_val & 0x3FFF));

    struct result reg = udp_register(ephemeral_port, dns_udp_handler);
    if (reg.is_error) {
        dns_set_in_flight(false);
        return result_ipv4_addr_error(reg.code);
    }

    /* Build query */
    pseudo_rand_u64(&rand_val);
    u16 tx_id = (u16) (rand_val & 0xFFFF);
    u8 query[DNS_MAX_PACKET];
    sz qlen = dns_build_query(hostname, tx_id, query, sizeof(query));
    if (qlen == 0) {
        udp_unregister(ephemeral_port);
        dns_set_in_flight(false);
        return result_ipv4_addr_error(EINVAL);
    }

    /* Send, retrying a few times on EAGAIN (ARP not yet resolved). */
    struct byte_buf *buf;
    struct result send_res = result_error(EAGAIN);
    for (i32 attempt = 0; attempt < 3; attempt++) {
        send_buf_clear(&sb);
        buf = send_buf_prepend(&sb, qlen);
        if (!buf) {
            udp_unregister(ephemeral_port);
            dns_set_in_flight(false);
            return result_ipv4_addr_error(ENOMEM);
        }
        assert(byte_buf_append(buf, byte_view_new(query, qlen)) == qlen);

        send_res = udp_send(nameserver, ephemeral_port, DNS_PORT, sb, arn);
        if (!send_res.is_error || send_res.code != EAGAIN)
            break;
        sleep_ms(time_ms_new(500));
    }
    if (send_res.is_error) {
        udp_unregister(ephemeral_port);
        dns_set_in_flight(false);
        return result_ipv4_addr_error(send_res.code);
    }

    struct wait_queue_entry wqe = {
        .task = sched_current_task(),
        .reason = WAIT_UNBLOCK_NONE,
    };
    list_init(&wqe.node);

    struct callout tmo;
    callout_init(&tmo);
    wqe.timeout_callout = &tmo;
    struct wait_timeout_arg tmo_arg = {
        .wq = &dns_waitq,
        .wqe = &wqe,
    };

    prepare_to_wait(&dns_waitq, &wqe);
    callout_set_ticks(&tmo, time_ms_to_ticks(3000), wait_timeout_fn, &tmo_arg);

    for (;;) {
        /* Check before yielding: the reply may have arrived between
         * prepare_to_wait and here, so wake_up had no sleeping waiter.
         */
        if (dns_response_is_ready() || wait_should_exit(wqe.reason))
            break;
        sched_yield_trap();
        if (dns_response_is_ready() || wait_should_exit(wqe.reason))
            break;
        prepare_to_wait(&dns_waitq, &wqe);
    }

    callout_cancel_sync(&tmo);
    finish_wait(&dns_waitq, &wqe);

    udp_unregister(ephemeral_port);

    if (!dns_response_is_ready()) {
        pr_debug(STR("DNS: query for \"%s\" timed out\n"), hostname);
        dns_set_in_flight(false);
        return result_ipv4_addr_error(ETIMEDOUT);
    }

    /* Snapshot the response buffer under the lock. */
    u8 local_buf[DNS_MAX_PACKET];
    sz local_len;
    u64 lk = spin_lock_irqsave(&dns_lock);
    local_len = dns_response_len;
    for (sz i = 0; i < local_len; i++)
        local_buf[i] = dns_response_buf[i];
    spin_unlock_irqrestore(&dns_lock, lk);

    dns_set_in_flight(false);

    struct ipv4_addr resolved;
    struct result parse_res =
        dns_parse_response(local_buf, local_len, tx_id, &resolved);
    if (parse_res.is_error) {
        pr_debug(STR("DNS: parse error %hu for \"%s\"\n"), parse_res.code,
                 hostname);
        return result_ipv4_addr_error(parse_res.code);
    }

    return result_ipv4_addr_ok(resolved);
}

/* Self-tests */

#include __INC_TEST(dns)
