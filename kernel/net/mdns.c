/* SPDX-License-Identifier: MIT */
/* Minimal mDNS responder (RFC 6762 + RFC 6763).
 *
 * Answers A-record (QTYPE=1) queries for "mazu.local" with the interface's
 * IPv4 address.  All responses are sent to the mDNS multicast group
 * (224.0.0.251:5353) with the "cache flush" bit set (RFC 6762 §11.3).
 *
 * DNS wire format used here (all big-endian):
 *
 *   Header  (12 bytes): id=0, QR|AA flags, qdcount=0, ancount=1, ns=0, ar=0
 *   Answer  (27 bytes): NAME inline (13) + TYPE (2) + CLASS (2) +
 *                       TTL (4) + RDLENGTH (2) + RDATA (4)
 *   Total:  39 bytes
 *
 * RFC 6762 §18.12 prohibits questions in multicast responses, so QDCOUNT=0 and
 * there is no Question section. The Answer NAME field uses full inline wire
 * labels (4+"mazu"+5+"local"+0 = 12 bytes) rather than a compression pointer.
 *
 * Incoming query NAMEs: compression pointers are rejected; all labels must be
 * inline (good enough for mDNS queriers in practice).
 */

#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/net/ip.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/netorder.h>
#include <mazu/net/udp.h>
#include <mazu/print.h>

#include "dns_wire.h"
#include "mdns.h"

/* DNS constants (DNS_FLAG_QR, DNS_QTYPE_A, DNS_QCLASS_IN in dns_wire.h) */
#define DNS_FLAG_AA 0x0400u /* authoritative answer */
#define DNS_QTYPE_ANY 255
#define DNS_QCLASS_FLUSH 0x8000u /* cache-flush bit (mDNS, RFC 6762 §11.3) */
#define DNS_TTL_SECONDS 120

/* mDNS multicast MAC: 01:00:5E:00:00:FB (derived from 224.0.0.251). */
static const struct mac_addr mdns_multicast_mac = {
    .addr = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb},
};

/* Hostname label constants

 * "mazu.local" in DNS wire format: length-prefixed labels, terminated by 0.
 */
static const u8 mdns_labels[] = {
    4, 'm', 'a', 'z', 'u', 5, 'l', 'o', 'c', 'a', 'l', 0,
};
#define MDNS_LABELS_LEN ((sz) sizeof(mdns_labels)) /* 13 bytes */

/* Skip a DNS QNAME (inline labels only; no pointer support in queries).
 * Returns the number of bytes consumed, or 0 on malformed input.
 */
static sz dns_skip_qname(const u8 *buf, sz len, sz off)
{
    sz orig_off = off;
    while (off < len) {
        u8 label_len = buf[off];
        if (label_len == 0)
            return off - orig_off + 1; /* include the terminal zero */
        if (label_len & 0xC0u) {
            /* Compression pointer (0xC0) or extended label type (0x40-0xBF,
             * RFC 6891) - neither expected in mDNS queries; reject.
             */
            return 0;
        }
        off += 1 + label_len;
    }
    return 0; /* truncated */
}

/* Compare the inline DNS QNAME at buf[off..] with the compile-time labels.
 * Returns true if they match exactly.
 */
static bool dns_qname_matches(const u8 *buf, sz len, sz off)
{
    if (off + MDNS_LABELS_LEN > len)
        return false;
    for (sz i = 0; i < MDNS_LABELS_LEN; i++) {
        u8 wire = buf[off + i];
        /* Case-insensitive comparison for label characters (RFC 1034 §3.1).
         * mdns_labels[] is already lowercase so only the wire byte needs
         * folding.
         */
        if (wire >= 'A' && wire <= 'Z')
            wire = wire - 'A' + 'a';
        if (wire != mdns_labels[i])
            return false;
    }
    return true;
}

/* Response builder

 * Build a minimal mDNS A-record response compliant with RFC 6762.
 *
 * RFC 6762 §18.1:  ID MUST be 0 in multicast responses.
 * RFC 6762 §18.12: QDCOUNT MUST be 0; questions MUST NOT appear in responses.
 *
 * Response layout (39 bytes fixed):
 *   12  header  (ID=0, QR|AA, QD=0, AN=1, NS=0, AR=0)
 *   13  NAME    (mdns_labels inline - length-prefixed labels + terminal 0)
 *    2  TYPE    (A = 1)
 *    2  CLASS   (IN=1 | cache-flush bit)
 *    4  TTL     (120 s)
 *    2  RDLEN   (4)
 *    4  RDATA   (IPv4 address)
 *
 *   39  total
 *
 * No compression pointer is used: there is no question section to point into.
 */
#define MDNS_RESPONSE_SIZE (12 + MDNS_LABELS_LEN + 2 + 2 + 4 + 2 + 4)

static sz mdns_build_response(struct ipv4_addr host_ip, u8 *out, sz out_cap)
{
    if (out_cap < MDNS_RESPONSE_SIZE)
        return 0;

    sz w = 0;

    /* Header */
    dns_write_u16(out + w, 0);
    w += 2; /* ID = 0 (RFC 6762 §18.1) */
    dns_write_u16(out + w, (u16) (DNS_FLAG_QR | DNS_FLAG_AA));
    w += 2; /* Flags */
    dns_write_u16(out + w, 0);
    w += 2; /* QDCOUNT = 0 */
    dns_write_u16(out + w, 1);
    w += 2; /* ANCOUNT = 1 */
    dns_write_u16(out + w, 0);
    w += 2; /* NSCOUNT = 0 */
    dns_write_u16(out + w, 0);
    w += 2; /* ARCOUNT = 0 */

    /* Answer section - NAME: inline labels (no compression pointer). */
    for (sz i = 0; i < MDNS_LABELS_LEN; i++)
        out[w++] = mdns_labels[i];
    /* TYPE = A */
    dns_write_u16(out + w, DNS_QTYPE_A);
    w += 2;
    /* CLASS = IN | cache-flush */
    dns_write_u16(out + w, (u16) (DNS_QCLASS_IN | DNS_QCLASS_FLUSH));
    w += 2;
    /* TTL */
    dns_write_u32(out + w, DNS_TTL_SECONDS);
    w += 4;
    /* RDLENGTH = 4 */
    dns_write_u16(out + w, 4);
    w += 2;
    /* RDATA: IPv4 address */
    out[w++] = host_ip.addr[0];
    out[w++] = host_ip.addr[1];
    out[w++] = host_ip.addr[2];
    out[w++] = host_ip.addr[3];

    return w;
}

/* Public entry point */

struct result mdns_handle_query(struct netdev *recv_dev,
                                struct ipv4_addr src_ip __unused,
                                struct byte_view payload,
                                struct send_buf sb,
                                struct arena tmp)
{
    struct netdev_info recv_info;
    netdev_get_info(recv_dev, &recv_info);

    /* Minimum DNS header is 12 bytes. */
    if (payload.len < 12)
        return result_ok();

    const u8 *msg = byte_view_ptr(payload);

    u16 flags = dns_read_u16(msg + 2);
    u16 qdcount = dns_read_u16(msg + 4);

    /* Ignore responses (QR bit = 1). */
    if (flags & DNS_FLAG_QR)
        return result_ok();

    /* Only standard queries are handled (OPCODE == 0). */
    if ((flags & 0x7800u) != 0)
        return result_ok();

    if (qdcount == 0)
        return result_ok();

    /* Walk question section looking for an A-record query for the hostname. */
    sz off = 12;
    bool found = false;

    for (u16 q = 0; q < qdcount && off < payload.len; q++) {
        sz name_start = off;
        sz name_len = dns_skip_qname(msg, payload.len, off);
        if (name_len == 0)
            return result_ok(); /* malformed */
        off += name_len;

        if (off + 4 > (sz) payload.len)
            return result_ok(); /* truncated */

        u16 qtype = dns_read_u16(msg + off);
        u16 qclass = dns_read_u16(msg + off + 2) & 0x7FFFu; /* strip QU bit */
        off += 4;

        if (qclass != DNS_QCLASS_IN)
            continue;
        if (qtype != DNS_QTYPE_A && qtype != DNS_QTYPE_ANY)
            continue;
        if (!dns_qname_matches(msg, payload.len, name_start))
            continue;

        found = true;
        break;
    }

    if (!found)
        return result_ok();

    /* Allocate scratch buffer for the DNS response (fixed 39 bytes). */
    u8 *resp_buf = arena_alloc(&tmp, MDNS_RESPONSE_SIZE);

    sz resp_len =
        mdns_build_response(recv_info.ip_addr, resp_buf, MDNS_RESPONSE_SIZE);
    if (resp_len == 0)
        return result_ok();

    /* Attach response bytes to the send buffer. */
    struct byte_buf *buf = send_buf_prepend(&sb, resp_len);
    if (!buf)
        return result_error(ENOMEM);
    if (byte_buf_append(buf, byte_view_new(resp_buf, resp_len)) !=
        (sz) resp_len)
        return result_error(ENOMEM);

    /* Prepend UDP header (src=5353, dst=5353). */
    struct udp_hdr {
        net_u16 src_port, dst_port, length, checksum;
    } __packed;

    struct udp_hdr udph = {
        .src_port = net_u16_from_u16(MDNS_PORT),
        .dst_port = net_u16_from_u16(MDNS_PORT),
        .length = net_u16_from_u16((u16) (sizeof(udph) + resp_len)),
        .checksum = net_u16_from_u16(0),
    };

    struct byte_buf *udp_buf = send_buf_prepend(&sb, sizeof(udph));
    if (!udp_buf)
        return result_error(ENOMEM);
    if (byte_buf_append(udp_buf, byte_view_new(&udph, sizeof(udph))) !=
        sizeof(udph))
        return result_error(ENOMEM);

    /* Prepend IPv4 header (src = host IP, dst = 224.0.0.251, TTL = 255). */
    struct ipv4_addr mdns_group = ipv4_addr_new(
        MDNS_MULTICAST_0, MDNS_MULTICAST_1, MDNS_MULTICAST_2, MDNS_MULTICAST_3);
    /* RFC 6762 §11: mDNS packets MUST be sent with IP TTL = 255. */
    struct result res = ipv4_prepend_header(recv_info.ip_addr, mdns_group,
                                            IPV4_PROTOCOL_UDP, 255, 0, &sb);
    if (res.is_error)
        return res;

    /* Send with the mDNS multicast MAC. */
    res = netdev_send(mdns_multicast_mac, recv_dev, NETDEV_PROTO_IPV4, sb);
    if (!res.is_error)
        pr_debug(
            STR("mDNS: answered A-record query "
                "for " MDNS_HOSTNAME "." MDNS_DOMAIN "\n"));

    return res;
}

/* UDP listener adapter

 * Adapter matching udp_handler_fn signature; forwards to mdns_handle_query.
 */
static struct result mdns_udp_handler(struct netdev *recv_dev,
                                      struct ipv4_addr src_ip,
                                      u16 src_port __unused,
                                      u16 dest_port __unused,
                                      struct byte_view payload,
                                      struct send_buf sb,
                                      struct arena tmp)
{
    return mdns_handle_query(recv_dev, src_ip, payload, sb, tmp);
}

static void mdns_init_hook(u32 lifecycle_flag __unused)
{
    struct result r = udp_register(MDNS_PORT, mdns_udp_handler);
    if (r.is_error)
        pr_warn(STR("mDNS: failed to register UDP port %hu\n"),
                (u16) MDNS_PORT);
    else
        pr_info(STR("mDNS: registered listener on port %hu\n"),
                (u16) MDNS_PORT);
}

INIT_TASK("mdns",
          mdns_init_hook,
          INIT_REQUIRES(INITGRAPH_STAGE_NET),
          INIT_ENTAILS(INITGRAPH_STAGE_SUBSYS),
          INIT_FLAG_PRIMARY);
