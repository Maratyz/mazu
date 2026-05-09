/* SPDX-License-Identifier: MIT */
/* DNS stub resolver - synchronous A-record queries over UDP.
 *
 * dns_build_query() and dns_parse_response() are pure functions on byte
 * buffers and can be tested in isolation.  dns_resolve() is the high-level
 * blocking API that sends a query and waits for the response.
 */

#ifndef MAZU_NET_DNS_H
#define MAZU_NET_DNS_H

#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/send_buf.h>

/* Build a standard A-record query for 'hostname' into 'out'.
 * 'tx_id' is the 16-bit transaction ID for matching responses.
 * Returns the number of bytes written, or 0 on error (hostname too long,
 * buffer too small).
 */
sz dns_build_query(struct str hostname, u16 tx_id, u8 *out, sz out_cap);

/* Parse a DNS response in 'data' (len bytes).  Verifies that the transaction
 * ID matches 'expected_tx_id'.  Extracts the first A record and writes it
 * to '*out_addr'.
 * Returns result_ok() on success, ENOENT for NXDOMAIN, EINVAL for
 * parse errors or ID mismatch.
 */
struct result dns_parse_response(const u8 *data,
                                 sz len,
                                 u16 expected_tx_id,
                                 struct ipv4_addr *out_addr);

/* Synchronous blocking resolver.  Sends an A-record query to 'nameserver'
 * and waits up to ~3 seconds for a response.
 * Returns the resolved IPv4 address or an error (ETIMEDOUT, ENOENT, etc.).
 */
struct result_ipv4_addr dns_resolve(struct ipv4_addr nameserver,
                                    struct str hostname,
                                    struct send_buf sb,
                                    struct arena arn);

#endif /* MAZU_NET_DNS_H */
