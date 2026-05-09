/* SPDX-License-Identifier: MIT */
#include <mazu/asm.h>
#include <mazu/kvalloc.h>
#include <mazu/net/ethernet.h>
#include <mazu/net/netdev.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>

/* Device registration and lookup */

#define NETDEV_TABLE_SIZE 16
static bool global_netdev_table_used[NETDEV_TABLE_SIZE];
static struct netdev *global_netdev_table[NETDEV_TABLE_SIZE];
static spinlock_t netdev_table_lock = SPINLOCK_INITIALIZER;

static struct ipv4_addr global_netdev_default_ip_addr;

static inline struct netdev *netdev_get_locked(struct netdev *dev)
{
    if (!dev)
        return NULL;
    __atomic_fetch_add(&dev->refcnt, 1, __ATOMIC_RELAXED);
    return dev;
}

static inline void netdev_meta_write_begin(struct netdev *dev)
{
    __atomic_fetch_add(&dev->meta_seq, 1, __ATOMIC_ACQ_REL);
}

static inline void netdev_meta_write_end(struct netdev *dev)
{
    __atomic_fetch_add(&dev->meta_seq, 1, __ATOMIC_RELEASE);
}

void netdev_set_default_ip_addr(struct ipv4_addr ip_addr)
{
    u64 flags = spin_lock_irqsave(&netdev_table_lock);
    global_netdev_default_ip_addr = ip_addr;
    spin_unlock_irqrestore(&netdev_table_lock, flags);
}

struct result netdev_register_device(struct netdev *dev)
{
    assert(dev);

    struct ipv4_addr zero = IPV4_ADDR_ANY;

    u64 flags = spin_lock_irqsave(&netdev_table_lock);

    if (ipv4_addr_is_equal(global_netdev_default_ip_addr, zero)) {
        spin_unlock_irqrestore(&netdev_table_lock, flags);
        return result_error(EINVAL);
    }

    /* New devices inherit the default IP address set by the network stack. */
    if (!ipv4_addr_is_equal(dev->ip_addr, zero)) {
        spin_unlock_irqrestore(&netdev_table_lock, flags);
        return result_error(EINVAL);
    }

    __atomic_store_n(&dev->meta_seq, 0, __ATOMIC_RELAXED);
    dev->ip_addr = global_netdev_default_ip_addr;
    __atomic_store_n(&dev->refcnt, 0, __ATOMIC_RELAXED);

    /* The network stack currently supports Ethernet devices only. */
    if (dev->link_type != NETDEV_LINK_TYPE_ETHERNET) {
        spin_unlock_irqrestore(&netdev_table_lock, flags);
        pr_err(STR("netdev: Unsupported link type %d (only Ethernet "
                   "supported)\n"),
               dev->link_type);
        return result_error(EINVAL);
    }
    dev->mtu = MAX(0, dev->mtu - sizeof(struct ethernet_frame_header));

    byte fmt_buf[2 * MAC_ADDR_FMT_BUF_SIZE + IP_ADDR_FMT_BUF_SIZE];
    struct arena fmt_arn = arena_new(byte_array_new(fmt_buf, countof(fmt_buf)));

    for (sz i = 0; i < NETDEV_TABLE_SIZE; i++) {
        if (global_netdev_table_used[i] &&
            mac_addr_is_equal(dev->mac_addr,
                              global_netdev_table[i]->mac_addr)) {
            spin_unlock_irqrestore(&netdev_table_lock, flags);
            pr_debug(STR("Device with MAC address %s already exists\n"),
                     mac_addr_format(dev->mac_addr, &fmt_arn));
            return result_error(EEXIST);
        }
    }

    for (sz i = 0; i < NETDEV_TABLE_SIZE; i++) {
        if (!global_netdev_table_used[i]) {
            global_netdev_table_used[i] = true;
            global_netdev_table[i] = dev;
            spin_unlock_irqrestore(&netdev_table_lock, flags);
            pr_info(STR("Registered device with MAC address %s and IP "
                        "address %s\n"),
                    mac_addr_format(dev->mac_addr, &fmt_arn),
                    ipv4_addr_format(dev->ip_addr, &fmt_arn));
            return result_ok();
        }
    }

    spin_unlock_irqrestore(&netdev_table_lock, flags);
    return result_error(ENOMEM);
}

void netdev_unregister_device(struct netdev *dev)
{
    assert(dev);

    u64 flags = spin_lock_irqsave(&netdev_table_lock);
    for (sz i = 0; i < NETDEV_TABLE_SIZE; i++) {
        if (global_netdev_table_used[i] && global_netdev_table[i] == dev) {
            global_netdev_table_used[i] = false;
            global_netdev_table[i] = NULL;
            spin_unlock_irqrestore(&netdev_table_lock, flags);
            /* Wait for all pinned references to be released before
             * returning, so the caller can safely free the device.
             */
            while (__atomic_load_n(&dev->refcnt, __ATOMIC_ACQUIRE) > 0)
                ;
            return;
        }
    }
    spin_unlock_irqrestore(&netdev_table_lock, flags);
}

struct netdev *netdev_lookup_ip_addr(struct ipv4_addr addr)
{
    /* Multiple devices can share an IP address. Return the last match and log
     * a warning so callers can diagnose ambiguous routing decisions.
     */
    sz n_matches = 0;
    struct netdev *last_match = NULL;

    u64 flags = spin_lock_irqsave(&netdev_table_lock);
    for (sz i = 0; i < NETDEV_TABLE_SIZE; i++) {
        if (global_netdev_table_used[i] &&
            ipv4_addr_is_equal(addr, global_netdev_table[i]->ip_addr)) {
            n_matches++;
            last_match = global_netdev_table[i];
        }
    }
    last_match = netdev_get_locked(last_match);
    spin_unlock_irqrestore(&netdev_table_lock, flags);

    if (n_matches > 1) {
        byte backing[32];
        struct arena tmp = arena_new(byte_array_new(backing, sizeof(backing)));
        pr_warn(STR("Found more than one device for IP address %s. Returning "
                    "the last one\n"),
                ipv4_addr_format(addr, &tmp));
    }

    return last_match;
}

struct netdev *netdev_lookup_mac_addr(struct mac_addr addr)
{
    /* MAC addresses are unique within the global netdev table. */
    u64 flags = spin_lock_irqsave(&netdev_table_lock);
    for (sz i = 0; i < NETDEV_TABLE_SIZE; i++) {
        if (global_netdev_table_used[i] &&
            mac_addr_is_equal(addr, global_netdev_table[i]->mac_addr)) {
            struct netdev *dev = netdev_get_locked(global_netdev_table[i]);
            spin_unlock_irqrestore(&netdev_table_lock, flags);
            return dev;
        }
    }
    spin_unlock_irqrestore(&netdev_table_lock, flags);

    return NULL;
}

void netdev_put(struct netdev *netdev)
{
    assert(netdev);
    __atomic_fetch_sub(&netdev->refcnt, 1, __ATOMIC_RELEASE);
}

void netdev_get_info(struct netdev *netdev, struct netdev_info *out)
{
    assert(netdev);
    assert(out);

    for (;;) {
        u32 seq0 = __atomic_load_n(&netdev->meta_seq, __ATOMIC_ACQUIRE);
        if (seq0 & 1)
            continue;

        out->mac_addr = mac_addr_new(
            __atomic_load_n(&netdev->mac_addr.addr[0], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->mac_addr.addr[1], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->mac_addr.addr[2], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->mac_addr.addr[3], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->mac_addr.addr[4], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->mac_addr.addr[5], __ATOMIC_RELAXED));
        out->ip_addr = ipv4_addr_new(
            __atomic_load_n(&netdev->ip_addr.addr[0], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->ip_addr.addr[1], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->ip_addr.addr[2], __ATOMIC_RELAXED),
            __atomic_load_n(&netdev->ip_addr.addr[3], __ATOMIC_RELAXED));
        out->mtu = __atomic_load_n(&netdev->mtu, __ATOMIC_RELAXED);

        u32 seq1 = __atomic_load_n(&netdev->meta_seq, __ATOMIC_ACQUIRE);
        if (seq0 == seq1)
            return;
    }
}

void netdev_set_ip_addr(struct netdev *netdev, struct ipv4_addr ip_addr)
{
    assert(netdev);
    netdev_meta_write_begin(netdev);
    netdev->ip_addr = ip_addr;
    netdev_meta_write_end(netdev);
}

/* Send data */

static struct result netdev_append_link_header(struct byte_buf *buf,
                                               struct netdev *netdev,
                                               struct mac_addr dest_mac,
                                               netdev_proto_t proto)
{
    /* Link-layer header construction currently supports Ethernet only. */
    if (netdev->link_type != NETDEV_LINK_TYPE_ETHERNET) {
        pr_err(STR("netdev: Cannot append Ethernet header to link type "
                   "%d\n"),
               netdev->link_type);
        return result_error(EINVAL);
    }

    struct option_u16 ether_type_opt = ethernet_type_from_netdev_proto(proto);
    if (ether_type_opt.is_none)
        return result_error(EINVAL);

    struct ethernet_frame_header ether_hdr = {
        .dest = dest_mac,
        .src = netdev->mac_addr,
        .ether_type = net_u16_from_u16(option_u16_checked(ether_type_opt)),
    };

    sz n_appended =
        byte_buf_append(buf, byte_view_new(&ether_hdr, sizeof(ether_hdr)));
    if (n_appended != sizeof(ether_hdr))
        return result_error(ENOMEM);

    return result_ok();
}

struct result netdev_send(struct mac_addr dest_mac,
                          struct netdev *netdev,
                          netdev_proto_t proto,
                          struct send_buf sb)
{
    /* Reject unsupported link types before touching the send buffer. */
    if (netdev->link_type != NETDEV_LINK_TYPE_ETHERNET) {
        pr_err(STR("netdev: Cannot send on link type %d (Ethernet "
                   "required)\n"),
               netdev->link_type);
        return result_error(EINVAL);
    }
    struct byte_buf *buf =
        send_buf_prepend(&sb, sizeof(struct ethernet_frame_header));
    if (!buf)
        return result_error(ENOMEM);

    struct result res = netdev_append_link_header(buf, netdev, dest_mac, proto);
    if (res.is_error)
        return res;

    return netdev->send_frame(netdev, sb);
}

/* Input (receive) queue */

#define NETDEV_INPUT_PACKET_SIZE 2048
#define NETDEV_INPUT_QUEUE_SIZE 64
static_assert((NETDEV_INPUT_QUEUE_SIZE & (NETDEV_INPUT_QUEUE_SIZE - 1)) == 0,
              "NETDEV_INPUT_QUEUE_SIZE must be a power of two for O(1) "
              "wrap-around");
static struct input_packet global_input_queue[NETDEV_INPUT_QUEUE_SIZE];
static sz global_input_queue_tail;
static sz global_input_queue_head;
static bool global_input_queue_is_initialized;
static spinlock_t input_queue_producer_lock = SPINLOCK_INITIALIZER;

/* Ring-buffer semantics:
 * head points to the next free slot for an incoming packet.
 * tail points to the oldest packet that has not been released yet.
 * The queue is empty when head == tail and full when advancing head would
 * collide with tail.
 */

struct result netdev_init_input_queue(void)
{
    /* Allocate one persistent data buffer per queue entry. New packets replace
     * buffer contents in place.
     */
    for (sz i = 0; i < NETDEV_INPUT_QUEUE_SIZE; i++) {
        struct option_byte_array alloc =
            kvalloc_alloc(NETDEV_INPUT_PACKET_SIZE, 64);
        if (alloc.is_none) {
            for (sz j = 0; j < i; j++)
                kvalloc_free(byte_array_new(global_input_queue[j].data.dat,
                                            global_input_queue[j].data.cap));
            return result_error(ENOMEM);
        }
        global_input_queue[i].data =
            byte_buf_from_array(option_byte_array_checked(alloc));
    }

    global_input_queue_tail = 0;
    global_input_queue_head = 0;

    global_input_queue_is_initialized = true;

    return result_ok();
}

static struct result netdev_intr_input_queue_add(struct mac_addr src,
                                                 struct netdev *netdev,
                                                 netdev_proto_t proto,
                                                 struct byte_view data)
{
    u64 flags = spin_lock_irqsave(&input_queue_producer_lock);

    sz head = __atomic_load_n(&global_input_queue_head, __ATOMIC_RELAXED);
    sz tail = __atomic_load_n(&global_input_queue_tail, __ATOMIC_ACQUIRE);

    if (((head + 1) & (NETDEV_INPUT_QUEUE_SIZE - 1)) == tail) {
        spin_unlock_irqrestore(&input_queue_producer_lock, flags);
        return result_error(EAGAIN);
    }

    struct input_packet *pkt = &global_input_queue[head];
    pkt->src = src;
    pkt->netdev = netdev;
    pkt->proto = proto;
    pkt->n_failed_to_handle = 0;
    pkt->data.len = 0;
    byte_buf_append(&pkt->data, data);

    __atomic_store_n(&global_input_queue_head,
                     (head + 1) & (NETDEV_INPUT_QUEUE_SIZE - 1),
                     __ATOMIC_RELEASE);

    spin_unlock_irqrestore(&input_queue_producer_lock, flags);
    return result_ok();
}

static void netdev_intr_receive_ethernet(struct netdev *netdev,
                                         struct byte_view frame)
{
    assert(netdev);

    /* Frame must be large enough to fit the ethernet header. */
    if (frame.len < sizeof(struct ethernet_frame_header))
        return;

    struct ethernet_frame_header *ether_hdr = byte_view_ptr(frame);

    /* Drop packets with a different destination address than the MAC address of
     * the netdev.
     */
    if (!mac_addr_is_equal(ether_hdr->dest, netdev->mac_addr) &&
        !mac_addr_is_equal(ether_hdr->dest, MAC_ADDR_BROADCAST))
        return;

    struct byte_view payload =
        byte_view_skip(frame, sizeof(struct ethernet_frame_header));

    struct option_netdev_proto_t proto_opt = netdev_proto_from_ethernet_type(
        u16_from_net_u16(ether_hdr->ether_type));
    if (proto_opt.is_none)
        return;

    netdev_intr_input_queue_add(ether_hdr->src, netdev,
                                option_netdev_proto_t_checked(proto_opt),
                                payload);
}

void netdev_intr_receive(struct netdev *netdev, struct byte_view frame)
{
    assert(global_input_queue_is_initialized);

    /* The receive path currently handles Ethernet frames only. */
    if (netdev->link_type != NETDEV_LINK_TYPE_ETHERNET) {
        pr_warn(STR("netdev: Dropping frame from unsupported link type %d\n"),
                netdev->link_type);
        return;
    }

    netdev_intr_receive_ethernet(netdev, frame);
}

struct input_packet *netdev_get_input(void)
{
    assert(global_input_queue_is_initialized);

    /* Interrupts must not fire while examining the head index because the head
     * index is incremented in the receive interrupt handler.  Use save/restore
     * to preserve the caller's interrupt state.
     */
    u64 saved = intr_disable();

    sz tail = __atomic_load_n(&global_input_queue_tail, __ATOMIC_RELAXED);
    sz head = __atomic_load_n(&global_input_queue_head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        intr_restore(saved);
        return NULL; /* The queue is empty. */
    }

    /* Interrupts can be restored while processing the packet because the
     * receive interrupt handler won't modify the tail index.
     */
    intr_restore(saved);

    return &global_input_queue[tail];
}

void netdev_release_input(struct input_packet *pkt)
{
    assert(global_input_queue_is_initialized);

    sz tail = __atomic_load_n(&global_input_queue_tail, __ATOMIC_RELAXED);
    assert(pkt == &global_input_queue[tail]);

    /* Interrupts must not fire while updating the tail index because the update
     * depends on the state of the head index, but the head index is incremented
     * in the receive interrupt handler.  Use save/restore to preserve the
     * caller's interrupt state.
     */
    u64 saved = intr_disable();

    sz head = __atomic_load_n(&global_input_queue_head, __ATOMIC_ACQUIRE);

    if (tail == head) {
        intr_restore(saved);
        return;
    }

    __atomic_store_n(&global_input_queue_tail,
                     (tail + 1) & (NETDEV_INPUT_QUEUE_SIZE - 1),
                     __ATOMIC_RELEASE);

    intr_restore(saved);
}
