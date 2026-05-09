/* SPDX-License-Identifier: MIT */
/* Generic UDP substrate: listener registration, dispatch, and send path.
 *
 * Subsystems register handlers for specific destination ports via udp_register.
 */

#include <mazu/byte.h>
#include <mazu/net/ip.h>
#include <mazu/net/netdev.h>
#include <mazu/net/udp.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>

struct udp_header {
    net_u16 src_port;
    net_u16 dest_port;
    net_u16 length;   /* total length including this header */
    net_u16 checksum; /* optional in IPv4; set to 0 */
} __packed;

static_assert(sizeof(struct udp_header) == 8, "unexpected udp_header size");

/* Listener table */

#define UDP_MAX_LISTENERS 16

struct udp_listener {
    u16 port;
    udp_handler_fn handler;
};

static struct udp_listener udp_listeners[UDP_MAX_LISTENERS];
static sz udp_listener_count;
static spinlock_t udp_lock = SPINLOCK_INITIALIZER;

struct result udp_register(u16 port, udp_handler_fn handler)
{
    assert(handler);

    u64 flags = spin_lock_irqsave(&udp_lock);

    for (sz i = 0; i < udp_listener_count; i++) {
        if (udp_listeners[i].port == port) {
            spin_unlock_irqrestore(&udp_lock, flags);
            return result_error(EADDRINUSE);
        }
    }

    if (udp_listener_count >= UDP_MAX_LISTENERS) {
        spin_unlock_irqrestore(&udp_lock, flags);
        return result_error(ENOMEM);
    }

    udp_listeners[udp_listener_count].port = port;
    udp_listeners[udp_listener_count].handler = handler;
    udp_listener_count++;

    spin_unlock_irqrestore(&udp_lock, flags);
    return result_ok();
}

void udp_unregister(u16 port)
{
    u64 flags = spin_lock_irqsave(&udp_lock);

    for (sz i = 0; i < udp_listener_count; i++) {
        if (udp_listeners[i].port == port) {
            /* Swap with last entry to keep the table compact. */
            udp_listeners[i] = udp_listeners[udp_listener_count - 1];
            udp_listener_count--;
            spin_unlock_irqrestore(&udp_lock, flags);
            /* After removing the entry under udp_lock, no new dispatch for
             * this port can start (lookup is lock-serialized).  An in-flight
             * handler on another hart has its own function pointer copy and
             * synchronizes shared state through its own locks (e.g. dns_lock).
             */
            return;
        }
    }

    spin_unlock_irqrestore(&udp_lock, flags);
}

/* Receive path */

struct result udp_handle_packet(struct netdev *recv_dev,
                                struct ipv4_addr src_ip,
                                u16 src_port,
                                u16 dest_port,
                                struct byte_view payload,
                                struct send_buf sb,
                                struct arena tmp)
{
    /* Look up the handler under udp_lock so that udp_unregister() guarantees
     * no in-flight dispatch after it returns.  The handler itself runs outside
     * the lock — only the lookup is serialized.
     */
    udp_handler_fn handler = NULL;
    u64 flags = spin_lock_irqsave(&udp_lock);
    for (sz i = 0; i < udp_listener_count; i++) {
        if (udp_listeners[i].port == dest_port) {
            handler = udp_listeners[i].handler;
            break;
        }
    }
    spin_unlock_irqrestore(&udp_lock, flags);

    if (handler)
        return handler(recv_dev, src_ip, src_port, dest_port, payload, sb, tmp);

    pr_debug(STR("UDP: received datagram on port %hu (no handler). Dropping "
                 "...\n"),
             dest_port);
    return result_ok();
}

/* Send path */

static struct result udp_prepend_header(u16 src_port,
                                        u16 dest_port,
                                        struct send_buf *sb)
{
    sz total = sizeof(struct udp_header) + send_buf_total_length(*sb);
    if (total > 0xFFFF)
        return result_error(EMSGSIZE);

    struct udp_header hdr = {
        .src_port = net_u16_from_u16(src_port),
        .dest_port = net_u16_from_u16(dest_port),
        .length = net_u16_from_u16((u16) total),
        .checksum = net_u16_from_u16(0),
    };

    struct byte_buf *buf = send_buf_prepend(sb, sizeof(hdr));
    if (!buf)
        return result_error(ENOMEM);

    assert(byte_buf_append(buf, byte_view_new(&hdr, sizeof(hdr))) ==
           sizeof(hdr));
    return result_ok();
}

struct result udp_send(struct ipv4_addr dest_ip,
                       u16 src_port,
                       u16 dest_port,
                       struct send_buf sb,
                       struct arena arn)
{
    struct result res = udp_prepend_header(src_port, dest_port, &sb);
    if (res.is_error)
        return res;

    return ipv4_send_packet(dest_ip, IPV4_PROTOCOL_UDP, sb, arn);
}

struct result udp_send_broadcast(struct netdev *dev,
                                 u16 src_port,
                                 u16 dest_port,
                                 struct send_buf sb,
                                 struct arena arn __unused)
{
    assert(dev);

    struct result res = udp_prepend_header(src_port, dest_port, &sb);
    if (res.is_error)
        return res;

    res = ipv4_prepend_header(IPV4_ADDR_ANY, ipv4_addr_new(255, 255, 255, 255),
                              IPV4_PROTOCOL_UDP, 64, 0, &sb);
    if (res.is_error)
        return res;

    return netdev_send(MAC_ADDR_BROADCAST, dev, NETDEV_PROTO_IPV4, sb);
}

/* Self-tests */
#include __INC_TEST(udp)
