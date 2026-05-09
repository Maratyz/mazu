/* SPDX-License-Identifier: MIT */
#include <mazu/net/ip.h>
#include <mazu/selftest.h>

/* Test: PMTU cache insert, lookup, and update. */
static i32 test_pmtu_cache_basic(void)
{
    struct ipv4_addr dest = ipv4_addr_new(10, 0, 2, 15);

    /* Save cache state and clear it. */
    struct pmtu_entry saved[PMTU_CACHE_SIZE];
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        saved[i] = pmtu_cache[i];
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        pmtu_cache[i].valid = false;

    i32 rc = 0;

    u16 mtu = pmtu_update(dest, 1400);
    if (mtu != 1400) {
        rc = 1;
        goto out;
    }

    u16 cached = pmtu_lookup(dest);
    if (cached != 1400) {
        rc = 2;
        goto out;
    }

    mtu = pmtu_update(dest, 576);
    if (mtu != 576) {
        rc = 3;
        goto out;
    }

    cached = pmtu_lookup(dest);
    if (cached != 576) {
        rc = 4;
        goto out;
    }

    struct ipv4_addr unknown = ipv4_addr_new(172, 16, 0, 1);
    if (pmtu_lookup(unknown) != 0) {
        rc = 5;
        goto out;
    }

    mtu = pmtu_update(dest, 100);
    if (mtu != 576) {
        rc = 6;
        goto out;
    }

out:
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        pmtu_cache[i] = saved[i];

    return rc;
}
DEFINE_SELFTEST(pmtu_cache_basic, test_pmtu_cache_basic);

/* Test: expired PMTU entries are treated as cache misses on the lockless read
 * path and can be refreshed by the update path.
 */
static i32 test_pmtu_cache_expiry_snapshot(void)
{
    struct ipv4_addr dest = ipv4_addr_new(10, 0, 2, 16);
    u64 now = time_current_ms().ms;

    struct pmtu_entry saved[PMTU_CACHE_SIZE];
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        saved[i] = pmtu_cache[i];
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        pmtu_cache[i] = (struct pmtu_entry) {0};

    i32 rc = 0;

    pmtu_entry_set(&pmtu_cache[0], dest, 1400, now);
    if (pmtu_lookup(dest) != 1400) {
        rc = 1;
        goto out;
    }

    pmtu_entry_write_begin(&pmtu_cache[0]);
    pmtu_cache[0].updated_ms = now - PMTU_CACHE_AGE_MS;
    pmtu_entry_write_end(&pmtu_cache[0]);
    if (pmtu_lookup(dest) != 0) {
        rc = 2;
        goto out;
    }

    if (pmtu_update(dest, 1200) != 1200) {
        rc = 3;
        goto out;
    }
    if (pmtu_lookup(dest) != 1200) {
        rc = 4;
        goto out;
    }

out:
    for (sz i = 0; i < PMTU_CACHE_SIZE; i++)
        pmtu_cache[i] = saved[i];

    return rc;
}
DEFINE_SELFTEST(pmtu_cache_expiry_snapshot, test_pmtu_cache_expiry_snapshot);

/* Test: DF bit constant value. */
static i32 test_pmtu_df_bit(void)
{
    /* DF bit is bit 14 in the fragment_offset field (RFC 791). */
    if (IPV4_FLAG_DF != 0x4000)
        return 1;
    return 0;
}
DEFINE_SELFTEST(pmtu_df_bit, test_pmtu_df_bit);
