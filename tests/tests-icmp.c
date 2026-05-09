/* SPDX-License-Identifier: MIT */
#include <mazu/net/icmp.h>
#include <mazu/selftest.h>

static i32 test_ping_table(void)
{
    /* Save and clear the ping table. */
    struct ping_entry saved[PING_TABLE_SIZE];
    for (sz i = 0; i < PING_TABLE_SIZE; i++)
        saved[i] = ping_table[i];
    for (sz i = 0; i < PING_TABLE_SIZE; i++)
        ping_table[i].valid = false;

    /* Record a ping. */
    ping_table_record(0xCAFE, 1, 1000);
    u64 rtt = 0;
    if (ping_table_is_replied(0xCAFE, 1, &rtt))
        return 1; /* should not be replied yet */

    /* Complete it. */
    if (!ping_table_complete(0xCAFE, 1, 2000))
        return 2;
    if (!ping_table_is_replied(0xCAFE, 1, &rtt))
        return 3;

    /* RTT should be time_ticks_to_us(1000). */
    u64 expected_us = time_ticks_to_us(1000);
    if (rtt != expected_us)
        return 4;

    /* Clear it. */
    ping_table_clear(0xCAFE, 1);
    if (ping_table_is_replied(0xCAFE, 1, &rtt))
        return 5; /* should be gone */

    /* Restore. */
    for (sz i = 0; i < PING_TABLE_SIZE; i++)
        ping_table[i] = saved[i];

    return 0;
}

DEFINE_SELFTEST(ping_table, test_ping_table);
