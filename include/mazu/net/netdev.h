/* SPDX-License-Identifier: MIT */
/* Generic network device interface. */

#ifndef MAZU_NET_NETDEV_H
#define MAZU_NET_NETDEV_H

#include <mazu/byte.h>
#include <mazu/error.h>
#include <mazu/net/ip_addr.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/send_buf.h>
#include <mazu/option.h>

/* NOTE: The idea behind the 'NETDEV_*' constants is that they are independent
 * of any specific protocol. This means that numbers used by specific protocols
 * must be converted to the 'NETDEV_*' numbers. E.g., the Ethernet type field in
 * the Ethernet frame header must be converted to one of the 'NETDEV_PROTO_*'
 * constants.
 *
 * The reason that the 'NETDEV_*' constants don't simply count up starting at 0
 * or at 1 is that debugging is easier when working with more recognizable
 * numbers.
 */

typedef u16 netdev_proto_t;
struct_option(netdev_proto_t, netdev_proto_t);

#define NETDEV_PROTO_ARP 0xaa
#define NETDEV_PROTO_IPV4 0x04

typedef u16 netdev_link_type_t;

#define NETDEV_LINK_TYPE_ETHERNET 0xe7

struct netdev;

typedef struct result (*send_frame_func_t)(struct netdev *dev,
                                           struct send_buf sb);

struct netdev {
    u32 meta_seq;
    struct mac_addr mac_addr;
    struct ipv4_addr ip_addr;
    netdev_link_type_t link_type;
    send_frame_func_t send_frame;
    sz mtu;
    u32 refcnt; /* transient pins returned by netdev_lookup_* */
    void *private_data;
};

struct netdev_info {
    struct mac_addr mac_addr;
    struct ipv4_addr ip_addr;
    sz mtu;
};

struct input_packet {
    struct netdev *netdev;
    sz n_failed_to_handle;
    struct byte_buf data;
    netdev_proto_t proto;
    struct mac_addr src;
};

/* Set a default IP address to use for all new devices. */
void netdev_set_default_ip_addr(struct ipv4_addr ip_addr);

/* Register a 'struct netdev' in the global network device table. Callers must
 * provide a unique MAC address, a zero IP address, and a populated
 * 'send_frame' callback. Registration assigns the current default IP address
 * to the device.
 *
 * The driver owns the memory behind 'dev' for the entire time the device
 * remains registered.
 */
struct result netdev_register_device(struct netdev *dev);
/* Remove a device from the global network device table. Drivers must call
 * this before freeing the memory behind 'dev'.
 */
void netdev_unregister_device(struct netdev *dev);

/* Look up a registered network device by IP address or MAC address.
 * Returned devices are pinned and must be released with netdev_put().
 * MAC-address lookups are expected to be unique.
 */
struct netdev *netdev_lookup_ip_addr(struct ipv4_addr ip_addr);
struct netdev *netdev_lookup_mac_addr(struct mac_addr mac_addr);
void netdev_put(struct netdev *netdev);
void netdev_get_info(struct netdev *netdev, struct netdev_info *out);
void netdev_set_ip_addr(struct netdev *netdev, struct ipv4_addr ip_addr);

/* Send a packet of data that uses the protocol 'proto'. The destination
 * hardware address is 'dest_mac' and the interface to send the packet from is
 * 'netdev'. The data for the packet should be in 'sb'.
 */
struct result netdev_send(struct mac_addr dest_mac,
                          struct netdev *netdev,
                          netdev_proto_t proto,
                          struct send_buf sb);

/* Initialize the input queue for received packets. Must be called before
 * calling any of the receive/input-related functions.
 */
struct result netdev_init_input_queue(void);

/* Receive a packet of data from a device driver. This function can be called
 * inside an interrupt handler. The packet will be added to the input queue.
 */
void netdev_intr_receive(struct netdev *netdev, struct byte_view data);

/* Try to get the first input packet from the input queue. Call
 * 'netdev_release_input' on the packet when you are done processing it.
 */
struct input_packet *netdev_get_input(void);

/* Remove the given packet from the input queue. This frees up the entry to
 * store a newly received packet in it.
 */
void netdev_release_input(struct input_packet *pkt);

#endif /* MAZU_NET_NETDEV_H */
