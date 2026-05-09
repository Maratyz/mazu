/* SPDX-License-Identifier: MIT */
/* Host tests for TCP sequence number arithmetic (RFC 793 section 3.3).
 *
 * Source: kernel/net/tcp.c lines 1135-1153 (seq_lt, seq_le, seq_gt, seq_geq).
 * These are pure functions with no kernel dependencies beyond base types.
 */

#include <mazu/base.h>

/* Inline copies of the seq helpers -- kept in sync with kernel/net/tcp.c. */
static inline bool seq_lt(u32 a, u32 b)
{
    return (i32) (a - b) < 0;
}

static inline bool seq_le(u32 a, u32 b)
{
    return (i32) (a - b) <= 0;
}

static inline bool seq_gt(u32 a, u32 b)
{
    return (i32) (a - b) > 0;
}

static inline bool seq_geq(u32 a, u32 b)
{
    return (i32) (a - b) >= 0;
}

/* Test: basic less-than with non-wrapping values. */
int test_seq_lt_basic(void)
{
    if (!seq_lt(100, 200))
        return 1;
    if (seq_lt(200, 100))
        return 1;
    if (seq_lt(100, 100))
        return 1;
    return 0;
}

/* Test: less-than across the u32 wraparound boundary.
 * 0xFFFFFFFF is "before" 0x00000001 in TCP sequence space.
 */
int test_seq_lt_wrap(void)
{
    if (!seq_lt(0xFFFFFFFF, 0x00000001))
        return 1;
    if (seq_lt(0x00000001, 0xFFFFFFFF))
        return 1;
    /* Large gap near the wraparound. */
    if (!seq_lt(0xFFFFFFF0, 0x00000010))
        return 1;
    return 0;
}

/* Test: less-than-or-equal with equal values. */
int test_seq_le_equal(void)
{
    if (!seq_le(42, 42))
        return 1;
    if (!seq_le(0xFFFFFFFF, 0xFFFFFFFF))
        return 1;
    if (!seq_le(0, 0))
        return 1;
    /* Normal ordering. */
    if (!seq_le(100, 200))
        return 1;
    if (seq_le(200, 100))
        return 1;
    return 0;
}

/* Test: greater-than basic cases. */
int test_seq_gt_basic(void)
{
    if (!seq_gt(200, 100))
        return 1;
    if (seq_gt(100, 200))
        return 1;
    if (seq_gt(100, 100))
        return 1;
    return 0;
}

/* Test: greater-than-or-equal with equal values. */
int test_seq_geq_equal(void)
{
    if (!seq_geq(42, 42))
        return 1;
    if (!seq_geq(200, 100))
        return 1;
    if (seq_geq(100, 200))
        return 1;
    return 0;
}

/* Test: wraparound boundary -- sequence numbers near 0 and near U32_MAX. */
int test_seq_wrap_boundary(void)
{
    /* 0x80000000 apart is the ambiguous midpoint; by convention it's "equal
     * distance" and signed difference is negative (INT32_MIN), so seq_lt
     * returns true.
     */
    u32 a = 0;
    u32 b = 0x80000000;
    if (!seq_lt(a, b))
        return 1;

    /* One less than the midpoint: unambiguously "before". */
    if (!seq_lt(a, 0x7FFFFFFF))
        return 1;

    /* One more than the midpoint: wraps to "after". */
    if (!seq_gt(a, 0x80000001))
        return 1;

    return 0;
}
