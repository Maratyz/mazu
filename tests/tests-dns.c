/* SPDX-License-Identifier: MIT */
#include <kernel/net/dns_wire.h>
#include <mazu/net/dns.h>
#include <mazu/selftest.h>

static i32 test_dns_build_query(void)
{
    u8 buf[512];
    sz n = dns_build_query(STR("example.com"), 0x1234, buf, sizeof(buf));
    if (n == 0)
        return 1;

    /* Check header: ID=0x1234 */
    if (dns_read_u16(buf) != 0x1234)
        return 2;
    /* Flags: RD=1 */
    if (dns_read_u16(buf + 2) != DNS_FLAG_RD)
        return 3;
    /* QDCOUNT=1 */
    if (dns_read_u16(buf + 4) != 1)
        return 4;

    /* QNAME at offset 12: 7 "example" 3 "com" 0 */
    if (buf[12] != 7)
        return 5;
    if (buf[13] != 'e' || buf[14] != 'x' || buf[15] != 'a')
        return 6;
    if (buf[20] != 3)
        return 7;
    if (buf[21] != 'c' || buf[22] != 'o' || buf[23] != 'm')
        return 8;
    if (buf[24] != 0)
        return 9;

    /* QTYPE=A (1), QCLASS=IN (1) at offsets 25-28 */
    if (dns_read_u16(buf + 25) != DNS_QTYPE_A)
        return 10;
    if (dns_read_u16(buf + 27) != DNS_QCLASS_IN)
        return 11;

    return 0;
}

DEFINE_SELFTEST(dns_build_query, test_dns_build_query);

static i32 test_dns_parse_response(void)
{
    /* Craft a minimal valid DNS response with one A record for 93.184.216.34.
     *
     * Header (12 bytes):
     *   ID=0xABCD, flags=0x8180 (QR+RD+RA), QD=1, AN=1, NS=0, AR=0
     *
     * Question (16 bytes):
     *   7 "example" 3 "com" 0  QTYPE=1 QCLASS=1
     *
     * Answer (16 bytes):
     *   name ptr -> offset 12 (0xC00C), TYPE=1, CLASS=1, TTL=300, RDLEN=4,
     *   RDATA=93.184.216.34
     */
    u8 resp[] = {
        /* Header */
        0xAB,
        0xCD, /* ID */
        0x81,
        0x80, /* flags: QR+RD+RA */
        0x00,
        0x01, /* QDCOUNT */
        0x00,
        0x01, /* ANCOUNT */
        0x00,
        0x00, /* NSCOUNT */
        0x00,
        0x00, /* ARCOUNT */
        /* Question: 7 "example" 3 "com" 0 */
        0x07,
        'e',
        'x',
        'a',
        'm',
        'p',
        'l',
        'e',
        0x03,
        'c',
        'o',
        'm',
        0x00,
        /* QTYPE=A, QCLASS=IN */
        0x00,
        0x01,
        0x00,
        0x01,
        /* Answer: name=ptr 0xC00C */
        0xC0,
        0x0C,
        /* TYPE=A */
        0x00,
        0x01,
        /* CLASS=IN */
        0x00,
        0x01,
        /* TTL=300 */
        0x00,
        0x00,
        0x01,
        0x2C,
        /* RDLENGTH=4 */
        0x00,
        0x04,
        /* RDATA: 93.184.216.34 */
        93,
        184,
        216,
        34,
    };

    struct ipv4_addr addr;
    struct result r = dns_parse_response(resp, sizeof(resp), 0xABCD, &addr);
    if (r.is_error)
        return 1;
    if (addr.addr[0] != 93 || addr.addr[1] != 184 || addr.addr[2] != 216 ||
        addr.addr[3] != 34)
        return 2;

    /* Wrong ID should fail */
    r = dns_parse_response(resp, sizeof(resp), 0x1111, &addr);
    if (!r.is_error || r.code != EINVAL)
        return 3;

    /* NXDOMAIN response */
    u8 nxdomain[] = {
        0xAB,
        0xCD,
        0x81,
        0x83, /* QR+RD+RA+RCODE=3 (NXDOMAIN) */
        0x00,
        0x01,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        /* Question */
        0x07,
        'e',
        'x',
        'a',
        'm',
        'p',
        'l',
        'e',
        0x03,
        'c',
        'o',
        'm',
        0x00,
        0x00,
        0x01,
        0x00,
        0x01,
    };
    r = dns_parse_response(nxdomain, sizeof(nxdomain), 0xABCD, &addr);
    if (!r.is_error || r.code != ENOENT)
        return 4;

    return 0;
}

DEFINE_SELFTEST(dns_parse_response, test_dns_parse_response);
