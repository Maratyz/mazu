/* SPDX-License-Identifier: MIT */
/* Definitions representing network byte order (big-endian) data. */

#ifndef MAZU_NET_NETORDER_H
#define MAZU_NET_NETORDER_H

#include <mazu/base.h>

/* For reference, consider 0x0a0b0c0d stored at address a:
 *
 *   Little endian: a[0] = 0x0d, a[1] = 0x0c, a[2] = 0x0b, a[3] = 0x0a
 *   Big endian:    a[0] = 0x0a, a[1] = 0x0b, a[2] = 0x0c, a[3] = 0x0d
 *
 * Little endian stores the least-significant byte first; big endian stores
 * the most-significant byte first. Network protocols use big endian.
 */

/* These are the same if one is byte-swapped, just for fun. */
#define ORDER_LITTLE_ENDIAN 0xaabb
#define ORDER_BIG_ENDIAN 0xbbaa

#define NET_BYTE_ORDER ORDER_BIG_ENDIAN

#if defined(__riscv)
#define SYSTEM_BYTE_ORDER ORDER_LITTLE_ENDIAN
#else
#error System byte order not defined
#endif

typedef struct net_u16 {
    u16 inner;
} __packed net_u16;
typedef struct net_u32 {
    u32 inner;
} __packed net_u32;
typedef struct net_u64 {
    u64 inner;
} __packed net_u64;

#if NET_BYTE_ORDER != SYSTEM_BYTE_ORDER
static inline u16 u16_from_net_u16(net_u16 value)
{
    return __builtin_bswap16(value.inner);
}

static inline u32 u32_from_net_u32(net_u32 value)
{
    return __builtin_bswap32(value.inner);
}

static inline u64 u64_from_net_u64(net_u64 value)
{
    return __builtin_bswap64(value.inner);
}

static inline net_u16 net_u16_from_u16(u16 value)
{
    return (net_u16) {.inner = __builtin_bswap16(value)};
}

static inline net_u32 net_u32_from_u32(u32 value)
{
    return (net_u32) {.inner = __builtin_bswap32(value)};
}

static inline net_u64 net_u64_from_u64(u64 value)
{
    return (net_u64) {.inner = __builtin_bswap64(value)};
}
#else /* NET_BYTE_ORDER == SYSTEM_BYTE_ORDER */
static inline u16 u16_from_net_u16(net_u16 value)
{
    return value.inner;
}

static inline u32 u32_from_net_u32(net_u32 value)
{
    return value.inner;
}

static inline u64 u64_from_net_u64(net_u64 value)
{
    return value.inner;
}

static inline net_u16 net_u16_from_u16(u16 value)
{
    return (net_u16) {.inner = value};
}

static inline net_u32 net_u32_from_u32(u32 value)
{
    return (net_u32) {.inner = value};
}

static inline net_u64 net_u64_from_u64(u64 value)
{
    return (net_u64) {.inner = value};
}
#endif

#endif /* MAZU_NET_NETORDER_H */
