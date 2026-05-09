/* SPDX-License-Identifier: MIT */
/* Minimal DHCPv4 client.
 *
 * Implements the four-message exchange:
 *   DISCOVER (broadcast) -> OFFER -> REQUEST (broadcast) -> ACK
 *
 * This is an early-boot bring-up helper, not a general runtime API. It runs
 * before sched_init() publishes a current task, so it is allowed to poll the
 * NIC input queue directly and use bounded time_current_ms() deadlines.
 */

#include <mazu/kvalloc.h>
#include <mazu/net/dhcp.h>
#include <mazu/net/ip.h>
#include <mazu/net/mac_addr.h>
#include <mazu/net/netdev.h>
#include <mazu/net/udp.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/time.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY 2

#define DHCP_FLAGS_BROADCAST 0x80 /* high byte of the 16-bit flags field */

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER 2
#define DHCP_MSG_REQUEST 3
#define DHCP_MSG_ACK 5

/* RFC 2131 section 2: fixed BOOTP message = 236 bytes; options follow. */
#define DHCP_FIXED_SIZE 236
#define DHCP_OPTIONS_SIZE 312
#define DHCP_MSG_SIZE (DHCP_FIXED_SIZE + DHCP_OPTIONS_SIZE) /* 548 */

/* Offsets within the BOOTP/DHCP fixed header. */
#define DHCP_OFF_OP 0
#define DHCP_OFF_HTYPE 1
#define DHCP_OFF_HLEN 2
#define DHCP_OFF_HOPS 3
#define DHCP_OFF_XID 4     /* 4 bytes, big-endian */
#define DHCP_OFF_FLAGS 10  /* 2 bytes */
#define DHCP_OFF_YIADDR 16 /* 4 bytes: "your" IP assigned by server */
#define DHCP_OFF_CHADDR 28 /* 16 bytes: client hardware (MAC) address */
#define DHCP_OFF_OPTIONS DHCP_FIXED_SIZE

/* RFC 2132 magic cookie (first 4 bytes of options field). */
#define DHCP_MAGIC_0 0x63
#define DHCP_MAGIC_1 0x82
#define DHCP_MAGIC_2 0x53
#define DHCP_MAGIC_3 0x63

/* DHCP option tags used by this client. */
#define DHCP_OPT_PAD 0
#define DHCP_OPT_REQ_IP 50
#define DHCP_OPT_MSGTYPE 53
#define DHCP_OPT_SERVER 54
#define DHCP_OPT_END 255

#define DHCP_TIMEOUT_MS 5000u /* per attempt */
#define DHCP_MAX_ATTEMPTS 3

static void dhcp_build_msg(byte *buf,
                           u8 msg_type,
                           struct mac_addr mac,
                           u32 xid,
                           struct ipv4_addr req_ip,
                           struct ipv4_addr server_id)
{
    for (sz i = 0; i < DHCP_MSG_SIZE; i++)
        buf[i] = 0;

    buf[DHCP_OFF_OP] = DHCP_OP_REQUEST;
    buf[DHCP_OFF_HTYPE] = 1; /* Ethernet */
    buf[DHCP_OFF_HLEN] = 6;  /* MAC address length */

    buf[DHCP_OFF_XID + 0] = (u8) (xid >> 24);
    buf[DHCP_OFF_XID + 1] = (u8) (xid >> 16);
    buf[DHCP_OFF_XID + 2] = (u8) (xid >> 8);
    buf[DHCP_OFF_XID + 3] = (u8) (xid);

    /* Request broadcast replies so the host receives them before having an IP.
     */
    buf[DHCP_OFF_FLAGS] = DHCP_FLAGS_BROADCAST;

    for (sz i = 0; i < 6; i++)
        buf[DHCP_OFF_CHADDR + i] = mac.addr[i];

    buf[DHCP_OFF_OPTIONS + 0] = DHCP_MAGIC_0;
    buf[DHCP_OFF_OPTIONS + 1] = DHCP_MAGIC_1;
    buf[DHCP_OFF_OPTIONS + 2] = DHCP_MAGIC_2;
    buf[DHCP_OFF_OPTIONS + 3] = DHCP_MAGIC_3;

    sz opt = DHCP_OFF_OPTIONS + 4;

    buf[opt++] = DHCP_OPT_MSGTYPE;
    buf[opt++] = 1;
    buf[opt++] = msg_type;

    if (msg_type == DHCP_MSG_REQUEST) {
        buf[opt++] = DHCP_OPT_REQ_IP;
        buf[opt++] = 4;
        buf[opt++] = req_ip.addr[0];
        buf[opt++] = req_ip.addr[1];
        buf[opt++] = req_ip.addr[2];
        buf[opt++] = req_ip.addr[3];

        buf[opt++] = DHCP_OPT_SERVER;
        buf[opt++] = 4;
        buf[opt++] = server_id.addr[0];
        buf[opt++] = server_id.addr[1];
        buf[opt++] = server_id.addr[2];
        buf[opt++] = server_id.addr[3];
    }

    buf[opt] = DHCP_OPT_END;
}

/* Parse a raw IPv4 packet as a DHCP reply. Returns true and fills *out_yiaddr
 * (and optionally *out_server_id) when the packet matches 'expected_xid',
 * 'expected_mac', and 'want_type'.
 */
static bool dhcp_parse_reply(const byte *pkt,
                             sz pkt_len,
                             u32 expected_xid,
                             u8 want_type,
                             struct mac_addr expected_mac,
                             struct ipv4_addr *out_yiaddr,
                             struct ipv4_addr *out_server_id)
{
    if (pkt_len < 20)
        return false;

    u8 ihl = (u8) ((pkt[0] & 0x0fu) * 4u);
    u8 proto = pkt[9];

    if (proto != IPV4_PROTOCOL_UDP)
        return false;
    if (ihl < 20 || pkt_len < (sz) ihl + 8)
        return false;

    const byte *udp = pkt + ihl;
    u16 sport = (u16) (((u16) udp[0] << 8) | udp[1]);
    u16 dport = (u16) (((u16) udp[2] << 8) | udp[3]);

    if (sport != DHCP_SERVER_PORT || dport != DHCP_CLIENT_PORT)
        return false;

    /* Need at least: BOOTP fixed header + magic cookie + type option + end. */
    if (pkt_len < (sz) ihl + 8 + DHCP_FIXED_SIZE + 4 + 3 + 1)
        return false;

    const byte *dhcp = udp + 8;

    if (dhcp[DHCP_OFF_OP] != DHCP_OP_REPLY)
        return false;

    /* Verify the reply is addressed to this client's MAC (security check). */
    for (sz ci = 0; ci < 6; ci++) {
        if (dhcp[DHCP_OFF_CHADDR + ci] != expected_mac.addr[ci])
            return false;
    }

    u32 xid = ((u32) dhcp[DHCP_OFF_XID + 0] << 24) |
              ((u32) dhcp[DHCP_OFF_XID + 1] << 16) |
              ((u32) dhcp[DHCP_OFF_XID + 2] << 8) |
              (u32) dhcp[DHCP_OFF_XID + 3];
    if (xid != expected_xid)
        return false;

    if (dhcp[DHCP_OFF_OPTIONS + 0] != DHCP_MAGIC_0 ||
        dhcp[DHCP_OFF_OPTIONS + 1] != DHCP_MAGIC_1 ||
        dhcp[DHCP_OFF_OPTIONS + 2] != DHCP_MAGIC_2 ||
        dhcp[DHCP_OFF_OPTIONS + 3] != DHCP_MAGIC_3)
        return false;

    struct ipv4_addr yiaddr =
        ipv4_addr_new(dhcp[DHCP_OFF_YIADDR + 0], dhcp[DHCP_OFF_YIADDR + 1],
                      dhcp[DHCP_OFF_YIADDR + 2], dhcp[DHCP_OFF_YIADDR + 3]);

    /* Walk the options TLV chain for message type and server identifier. */
    u8 msg_type = 0;
    struct ipv4_addr server_id = IPV4_ADDR_ANY;

    sz dhcp_end = (sz) (pkt_len - (sz) (dhcp - pkt));
    if (dhcp_end > DHCP_MSG_SIZE)
        dhcp_end = DHCP_MSG_SIZE;

    for (sz i = DHCP_OFF_OPTIONS + 4; i < dhcp_end;) {
        u8 tag = dhcp[i];
        if (tag == DHCP_OPT_END)
            break;
        if (tag == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= dhcp_end)
            break;

        u8 len = dhcp[i + 1];
        if (i + 2 + (sz) len > dhcp_end)
            break;

        if (tag == DHCP_OPT_MSGTYPE && len >= 1)
            msg_type = dhcp[i + 2];
        else if (tag == DHCP_OPT_SERVER && len >= 4)
            server_id = ipv4_addr_new(dhcp[i + 2], dhcp[i + 3], dhcp[i + 4],
                                      dhcp[i + 5]);

        i += 2u + (sz) len;
    }

    if (msg_type != want_type)
        return false;

    *out_yiaddr = yiaddr;
    if (out_server_id)
        *out_server_id = server_id;
    return true;
}

static struct result dhcp_send(struct netdev *dev,
                               const byte *msg_buf,
                               struct arena arn)
{
    struct option_byte_array sb_opt = kvalloc_alloc(0x4000, 64);
    if (sb_opt.is_none)
        return result_error(ENOMEM);
    struct byte_array sb_ba = option_byte_array_checked(sb_opt);
    struct send_buf sb = send_buf_new(arena_new(sb_ba));

    struct byte_buf *payload = send_buf_prepend(&sb, DHCP_MSG_SIZE);
    if (payload)
        byte_buf_append(payload,
                        byte_view_new((void *) msg_buf, DHCP_MSG_SIZE));

    struct result res =
        udp_send_broadcast(dev, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, sb, arn);
    kvalloc_free(sb_ba);
    return res;
}

struct result dhcp_boot_acquire(struct netdev *dev,
                                struct ipv4_addr *out_ip,
                                struct arena arn)
{
    assert(dev);
    assert(out_ip);

    struct netdev_info dev_info;
    netdev_get_info(dev, &dev_info);

    /* XID: MAC bytes 2-5 XOR-ed with the low 32 bits of the current timestamp.
     * The time component ensures a fresh XID on each boot so a lagging OFFER
     * from a previous session cannot be mistaken for a reply to this one.
     */
    u32 xid = (((u32) dev_info.mac_addr.addr[2] << 24) |
               ((u32) dev_info.mac_addr.addr[3] << 16) |
               ((u32) dev_info.mac_addr.addr[4] << 8) |
               (u32) dev_info.mac_addr.addr[5]) ^
              (u32) time_current_ms().ms;

    struct ipv4_addr yiaddr = IPV4_ADDR_ANY;
    struct ipv4_addr server_id = IPV4_ADDR_ANY;
    bool success = false;

    for (sz attempt = 0; attempt < DHCP_MAX_ATTEMPTS && !success; attempt++) {
        /* Phase 1: DISCOVER -> OFFER */
        {
            byte buf[DHCP_MSG_SIZE];
            dhcp_build_msg(buf, DHCP_MSG_DISCOVER, dev_info.mac_addr, xid,
                           IPV4_ADDR_ANY, IPV4_ADDR_ANY);
            struct result r = dhcp_send(dev, buf, arn);
            if (r.is_error)
                pr_warn(STR("DHCP: failed to send DISCOVER: %hu\n"), r.code);
        }

        bool got_offer = false;
        struct time_ms deadline =
            time_ms_new(time_current_ms().ms + DHCP_TIMEOUT_MS);

        while (!got_offer && time_current_ms().ms < deadline.ms) {
            struct input_packet *pkt = netdev_get_input();
            if (pkt) {
                if (pkt->proto == NETDEV_PROTO_IPV4)
                    got_offer = dhcp_parse_reply(
                        pkt->data.dat, pkt->data.len, xid, DHCP_MSG_OFFER,
                        dev_info.mac_addr, &yiaddr, &server_id);
                netdev_release_input(pkt);
            }
        }

        if (!got_offer) {
            pr_info(STR("DHCP: no OFFER received (attempt %ld/%d)\n"),
                    attempt + 1, DHCP_MAX_ATTEMPTS);
            continue;
        }

        pr_info(STR("DHCP: got OFFER for %s\n"),
                ipv4_addr_format(yiaddr, &arn));

        /* Phase 2: REQUEST -> ACK */
        {
            byte buf[DHCP_MSG_SIZE];
            dhcp_build_msg(buf, DHCP_MSG_REQUEST, dev_info.mac_addr, xid,
                           yiaddr, server_id);
            struct result r = dhcp_send(dev, buf, arn);
            if (r.is_error)
                pr_warn(STR("DHCP: failed to send REQUEST: %hu\n"), r.code);
        }

        deadline = time_ms_new(time_current_ms().ms + DHCP_TIMEOUT_MS);

        while (!success && time_current_ms().ms < deadline.ms) {
            struct input_packet *pkt = netdev_get_input();
            if (pkt) {
                if (pkt->proto == NETDEV_PROTO_IPV4) {
                    struct ipv4_addr ack_ip;
                    if (dhcp_parse_reply(pkt->data.dat, pkt->data.len, xid,
                                         DHCP_MSG_ACK, dev_info.mac_addr,
                                         &ack_ip, NULL)) {
                        yiaddr = ack_ip;
                        success = true;
                    }
                }
                netdev_release_input(pkt);
            }
        }

        if (!success)
            pr_info(STR("DHCP: no ACK received (attempt %ld/%d)\n"),
                    attempt + 1, DHCP_MAX_ATTEMPTS);
    }

    if (!success)
        return result_error(ETIMEDOUT);

    pr_info(STR("DHCP: acquired %s\n"), ipv4_addr_format(yiaddr, &arn));
    *out_ip = yiaddr;
    return result_ok();
}
