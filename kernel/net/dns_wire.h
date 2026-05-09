/* SPDX-License-Identifier: MIT */
/* DNS wire format helpers shared by dns.c and mdns.c.
 *
 * These operate on raw u8 buffers in big-endian (network) byte order,
 * independent of the typed net_u16/net_u32 wrappers in netorder.h.
 */

#ifndef MAZU_NET_DNS_WIRE_H
#define MAZU_NET_DNS_WIRE_H

#include <mazu/base.h>

/* Common DNS constants used by both dns.c and mdns.c. */
#define DNS_FLAG_QR 0x8000u /* query/response flag */
#define DNS_QTYPE_A 1       /* A record (IPv4 address) */
#define DNS_QCLASS_IN 1     /* Internet class */

static inline u16 dns_read_u16(const u8 *p)
{
    return (u16) (((u16) p[0] << 8) | p[1]);
}

static inline void dns_write_u16(u8 *p, u16 v)
{
    p[0] = (u8) (v >> 8);
    p[1] = (u8) (v);
}

static inline void dns_write_u32(u8 *p, u32 v)
{
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) (v);
}

#endif /* MAZU_NET_DNS_WIRE_H */
