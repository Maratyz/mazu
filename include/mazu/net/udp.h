/* SPDX-License-Identifier: MIT */
/* Generic UDP listener table and send path.
 * Compiled when CONFIG_NET_UDP is enabled.
 */

#ifndef MAZU_NET_UDP_H
#define MAZU_NET_UDP_H

#include <mazu/arena.h>
#include <mazu/byte.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/send_buf.h>

/* Handler signature for UDP listeners.
 * recv_dev  - interface the datagram arrived on.
 * src_ip    - sender's IPv4 address.
 * src_port  - sender's UDP port.
 * dest_port - destination port (the one this handler was registered for).
 * payload   - raw UDP payload (after the 8-byte UDP header).
 * sb        - send buffer for any response.
 * tmp       - scratch arena.
 */
typedef struct result (*udp_handler_fn)(struct netdev *recv_dev,
                                        struct ipv4_addr src_ip,
                                        u16 src_port,
                                        u16 dest_port,
                                        struct byte_view payload,
                                        struct send_buf sb,
                                        struct arena tmp);

/* Register a listener on 'port'.  Returns EADDRINUSE if the port is
 * already registered, ENOMEM if the table is full.
 */
struct result udp_register(u16 port, udp_handler_fn handler);

/* Unregister the listener on 'port'.  No-op if 'port' is not registered. */
void udp_unregister(u16 port);

/* Called by ip.c when a UDP datagram arrives.
 * 'dest_port' is extracted from the UDP header by the caller.
 */
struct result udp_handle_packet(struct netdev *recv_dev,
                                struct ipv4_addr src_ip,
                                u16 src_port,
                                u16 dest_port,
                                struct byte_view payload,
                                struct send_buf sb,
                                struct arena tmp);

/* Send a unicast UDP datagram via the routing table and ARP. */
struct result udp_send(struct ipv4_addr dest_ip,
                       u16 src_port,
                       u16 dest_port,
                       struct send_buf sb,
                       struct arena arn);

/* Send a broadcast UDP datagram (src=0.0.0.0, dst=255.255.255.255) directly
 * on 'dev', bypassing the routing table and ARP.  Used by DHCP before an IP
 * address has been assigned to the interface.
 */
struct result udp_send_broadcast(struct netdev *dev,
                                 u16 src_port,
                                 u16 dest_port,
                                 struct send_buf sb,
                                 struct arena arn);

#endif /* MAZU_NET_UDP_H */
