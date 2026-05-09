/* SPDX-License-Identifier: MIT */
/* Minimal DHCPv4 client: blocking boot-time address acquisition.
 *
 * This helper is intentionally restricted to early boot before sched_init().
 * It is not a general runtime API: it polls the NIC input queue directly and
 * uses bounded wall-clock deadlines to bring up the first IPv4 address.
 */

#ifndef MAZU_NET_DHCP_H
#define MAZU_NET_DHCP_H

#include <mazu/arena.h>
#include <mazu/error.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/netdev.h>

/* Run the DHCP DISCOVER/OFFER/REQUEST/ACK exchange during early boot only.
 *
 * On success writes the assigned address into *out_ip and returns result_ok().
 * On timeout (DHCP_MAX_ATTEMPTS exhausted) returns result_error(ETIMEDOUT).
 * The caller should fall back to the static address from config.txt on failure.
 *
 * Must be called before sched_init() publishes a current task. 'arn' is used
 * for small temporary format-string allocations during logging.
 */
struct result dhcp_boot_acquire(struct netdev *dev,
                                struct ipv4_addr *out_ip,
                                struct arena arn);

#endif /* MAZU_NET_DHCP_H */
