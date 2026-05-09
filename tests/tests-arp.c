/* SPDX-License-Identifier: MIT */
#include <mazu/net/arp.h>
#include <mazu/selftest.h>
#include <mazu/time.h>

/* Test: ARP defend interval constant is reasonable (1-60 seconds). */
static i32 test_arp_defend_rate_limit(void)
{
    if (ARP_DEFEND_INTERVAL_MS < 1000 || ARP_DEFEND_INTERVAL_MS > 60000)
        return 1;
    return 0;
}

DEFINE_SELFTEST(arp_defend_rate_limit, test_arp_defend_rate_limit);

static i32 test_arp_lookup_expired_snapshot(void)
{
    struct arp_table_ent saved[GLOBAL_ARP_TABLE_SIZE];
    struct ipv4_addr ip = ipv4_addr_new(10, 0, 2, 20);
    struct mac_addr mac = mac_addr_new(0x02, 0x00, 0x00, 0x00, 0x00, 0x20);
    u64 now = time_current_ms().ms;

    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++)
        saved[i] = global_arp_table[i];
    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++)
        global_arp_table[i] = (struct arp_table_ent) {0};

    i32 rc = 0;

    arp_table_ent_store(&global_arp_table[0], ip, mac, now);
    if (arp_lookup_mac_addr(ip).is_none) {
        rc = 1;
        goto out;
    }

    arp_table_ent_write_begin(&global_arp_table[0]);
    global_arp_table[0].added_ms = now - ARP_ENTRY_TTL_MS;
    arp_table_ent_write_end(&global_arp_table[0]);

    if (!arp_lookup_mac_addr(ip).is_none) {
        rc = 2;
        goto out;
    }
    if (!global_arp_table[0].is_used) {
        rc = 3;
        goto out;
    }

out:
    for (sz i = 0; i < GLOBAL_ARP_TABLE_SIZE; i++)
        global_arp_table[i] = saved[i];

    return rc;
}

DEFINE_SELFTEST(arp_lookup_expired_snapshot, test_arp_lookup_expired_snapshot);
