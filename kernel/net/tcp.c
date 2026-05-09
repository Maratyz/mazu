/* SPDX-License-Identifier: MIT */
#include <mazu/net/tcp.h>

#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/byte.h>
#include <mazu/eventlog.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/kvalloc.h>
#include <mazu/list.h>
#include <mazu/net/ip.h>
#include <mazu/net/netorder.h>
#include <mazu/pool.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/string.h>
#include <mazu/time.h>
#include "net_ops.h"

static bool global_tcp_is_initialized;

/* Two-level locking:
 *  1. tcp_pool_lock: coarse-grained. Protects the connection pool, hash table,
 *     accept queues, send/recv buffers, state transitions, and packet
 *     processing.
 *     Every public API function that touches shared connection state acquires
 *     this lock. Internal helpers assume it is already held.
 *  2. conn->lock: per-connection. Protects high-frequency metadata reads and
 *     updates (state polls, notify pointer, buffered-data counters, pending
 *     accept counters) that should not contend on tcp_pool_lock.
 *
 * Lock order, when both are needed: tcp_pool_lock -> conn->lock.
 *
 * Both locks use irqsave because the network RX trap/IRQ path also calls into
 * TCP from HIGH priority.
 */
static spinlock_t tcp_pool_lock = SPINLOCK_INITIALIZER;

/* Observability counters (item 22). */
static sz global_tcp_stats_bytes_tx;
static sz global_tcp_stats_bytes_rx;
static sz global_tcp_stats_retransmits;
static sz global_tcp_stats_pool_exhaustion;
static sz global_tcp_stats_n_connections;

/* SYN flood guard: max half-open (SYN_RCVD) connections from a single peer IP.
 */
#define TCP_MAX_SYN_RCVD_PER_IP 4

/* Per-source-IP connection limit: max simultaneous half-open + established
 * connections from any single peer IP. Protects the fixed-size connection pool
 * from a single client exhausting all slots. Rejected with RST so the client
 * gets immediate feedback (rather than a silent drop).
 */
#define TCP_MAX_CONNS_PER_IP 8

/* Hash-based connection demux. 256 buckets, Jenkins one-at-a-time hash seeded
 * at boot from rdtime low bits. Exact-match lookup is O(1) amortized; wildcard/
 * listen sockets fall back to linear scan (typically 1 socket).
 */
#define TCP_HASH_BUCKETS 256
#define TCP_HASH_MASK (TCP_HASH_BUCKETS - 1)

static struct list_head tcp_hash_table[TCP_HASH_BUCKETS];
static u32 tcp_hash_seed;

/* Jenkins one-at-a-time hash on the full 4-tuple + seed. */
static inline u32 tcp_hash_4tuple(struct ipv4_addr host_addr,
                                  u16 host_port,
                                  struct ipv4_addr peer_addr,
                                  u16 peer_port)
{
    u32 h = tcp_hash_seed;
    const u8 *key;

    /* host IP (4 bytes) */
    key = host_addr.addr;
    for (int i = 0; i < 4; i++) {
        h += key[i];
        h += h << 10;
        h ^= h >> 6;
    }

    /* host port (2 bytes, big-endian on wire, but hashed as native u16) */
    h += (u8) (host_port & 0xff);
    h += h << 10;
    h ^= h >> 6;
    h += (u8) (host_port >> 8);
    h += h << 10;
    h ^= h >> 6;

    /* peer IP (4 bytes) */
    key = peer_addr.addr;
    for (int i = 0; i < 4; i++) {
        h += key[i];
        h += h << 10;
        h ^= h >> 6;
    }

    /* peer port (2 bytes) */
    h += (u8) (peer_port & 0xff);
    h += h << 10;
    h ^= h >> 6;
    h += (u8) (peer_port >> 8);
    h += h << 10;
    h ^= h >> 6;

    /* Final avalanche */
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

struct tcp_header {
    net_u16 src_port;
    net_u16 dest_port;
    net_u32 seq_num;
    net_u32 ack_num;
#if SYSTEM_BYTE_ORDER == NET_BYTE_ORDER
    u8 header_len : 4;
    u8 reserved : 4;
#else  /* */
    u8 reserved : 4;
    u8 header_len : 4;
#endif /* TCP_MAX_SYN_RCVD_PER_IP */
    u8 flags;
    net_u16 window_size;
    net_u16 checksum;
    net_u16 urgent;
};

#define TCP_HDR_LEN_NO_OPT 5

#define TCP_HDR_FLAG_FIN BIT(0)
#define TCP_HDR_FLAG_SYN BIT(1)
#define TCP_HDR_FLAG_RST BIT(2)
#define TCP_HDR_FLAG_ACK BIT(4)

static_assert(sizeof(struct tcp_header) == 20, "unexpected tcp_header size");

#define TCP_OPT_EOL_KIND 0
#define TCP_OPT_NOP_KIND 1

/* TCP_OPT_*_LENGTH encodes the wire format length. The helper structs are used
 * only for constructing outgoing options, so trailing padding in the struct
 * does not change the on-wire option length.
 */

struct tcp_option_mss {
    u8 kind;
    u8 length;
    net_u16 value;
} __packed;

static_assert(sizeof(struct tcp_option_mss) == 4,
              "unexpected tcp_option_mss "
              "size");

#define TCP_OPT_MSS_KIND 2
#define TCP_OPT_MSS_LENGTH 4
#define TCP_OPT_MSS_VALUE 1460 /* Typical for ethernet with 1500 byte MTUs. */

struct tcp_option_ws {
    u8 kind, length, value;
    u8 nop; /* Padding */
} __packed;

static_assert(sizeof(struct tcp_option_ws) == 4,
              "unexpected tcp_option_ws "
              "size");

#define TCP_OPT_WS_KIND 3
#define TCP_OPT_WS_LENGTH 3

struct tcp_option_ts {
    u8 kind;
    u8 length;
    net_u32 tsval;
    net_u32 tsecr;
    u8 nop1;
    u8 nop2;
} __packed;

static_assert(sizeof(struct tcp_option_ts) == 12,
              "unexpected tcp_option_ts "
              "size");

#define TCP_OPT_TS_KIND 8
#define TCP_OPT_TS_LENGTH 10

#if CONFIG_TCP_SACK
#define TCP_OPT_SACK_PERMITTED_KIND 4   /* RFC 2018 */
#define TCP_OPT_SACK_PERMITTED_LENGTH 2 /* RFC 2018 */
#define TCP_OPT_SACK_KIND 5             /* RFC 2018 */
#define TCP_SACK_MAX_BLOCKS 3
#endif

/* Circular receive buffer

 * Power-of-2 ring buffer: use bitwise AND instead of modulo for wrapping.
 * On RISC-V, % compiles to a multi-cycle div/rem; & is always 1 cycle.
 */
static inline bool is_power_of_2(sz n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

static sz global_tcp_stats_recv_mem;

struct recv_buf {
    struct byte_array data;
    sz head;
    sz tail;
};

static inline sz recv_buf_count(struct recv_buf buf)
{
    assert(buf.data.len >= 0);
    if (buf.data.len == 0)
        return 0;
    DEBUG_ASSERT(is_power_of_2(buf.data.len));
    return (buf.head - buf.tail + buf.data.len) & (buf.data.len - 1);
}

static inline sz recv_buf_space(struct recv_buf buf)
{
    if (buf.data.len == 0)
        return 0;
    return buf.data.len - 1 - recv_buf_count(buf);
}

static struct result recv_buf_alloc(struct recv_buf *buf, struct pool *alloc)
{
    /* Security: Use ALWAYS_ASSERT for critical pointer validation - these
     * checks must remain active in production builds to prevent NULL pointer
     * dereferences that could lead to privilege escalation or DoS.
     */
    ALWAYS_ASSERT(buf != NULL);
    ALWAYS_ASSERT(alloc != NULL);

    /* Validate pool size is power of 2 for ring buffer operations */
    if (alloc->size <= 0 || !is_power_of_2(alloc->size)) {
        pr_debug(STR("recv_buf_alloc: invalid pool size %ld\n"), alloc->size);
        return result_error(EINVAL);
    }

    void *mem = pool_alloc(alloc);
    if (!mem)
        return result_error(ENOMEM);

    buf->data = byte_array_new(mem, alloc->size);
    buf->head = 0;
    buf->tail = 0;

    __atomic_fetch_add(&global_tcp_stats_recv_mem, alloc->size,
                       __ATOMIC_RELAXED);

    return result_ok();
}

static void recv_buf_free(struct recv_buf *buf, struct pool *alloc)
{
    assert(buf);
    assert(alloc);

    if (buf->data.dat) {
        pool_free(alloc, buf->data.dat);
        __atomic_fetch_sub(&global_tcp_stats_recv_mem, alloc->size,
                           __ATOMIC_RELAXED);
    }

    buf->data = byte_array_new(NULL, 0);
}

static struct result recv_buf_write(struct recv_buf *buf, struct byte_view data)
{
    /* Security: Always validate pointer and buffer bounds in production */
    ALWAYS_ASSERT(buf != NULL);

    /* Validate input data pointer */
    if (data.dat == NULL && data.len > 0) {
        pr_debug(
            STR("recv_buf_write: NULL data with non-zero "
                "length\n"));
        return result_error(EINVAL);
    }

    /* Security: Check for integer overflow in length comparison */
    if (data.len < 0) {
        pr_debug(STR("recv_buf_write: negative data length\n"));
        return result_error(EINVAL);
    }

    if (data.len > recv_buf_space(*buf))
        return result_error(EAGAIN);

    sz cap = buf->data.len;
    /* Security: Validate buffer capacity is valid for ring buffer operations */
    if (cap <= 0 || !is_power_of_2(cap)) {
        pr_debug(STR("recv_buf_write: invalid buffer capacity\n"));
        return result_error(EINVAL);
    }

    sz head = buf->head;
    /* Security: Validate head pointer is within bounds */
    if (head < 0 || head >= cap) {
        pr_debug(STR("recv_buf_write: head pointer out of bounds\n"));
        return result_error(EINVAL);
    }

    sz tail_space = cap - head;

    /* Security: Add bounds checking for memcpy operations to prevent buffer
     * overflow attacks via crafted TCP segments
     */
    if (data.len <= tail_space) {
        /* Verify destination region is within buffer bounds */
        if (head + data.len > cap) {
            pr_debug(
                STR("recv_buf_write: write would overflow "
                    "buffer\n"));
            return result_error(EINVAL);
        }
        memcpy(buf->data.dat + head, data.dat, data.len);
    } else {
        /* Wrap case: validate both memcpy operations */
        if (tail_space < 0 || tail_space > cap) {
            pr_debug(
                STR("recv_buf_write: invalid tail_space "
                    "calculation\n"));
            return result_error(EINVAL);
        }
        memcpy(buf->data.dat + head, data.dat, tail_space);

        /* wrap: avoid reading past source or writing past buffer start */
        if (data.len - tail_space <= 0 || data.len - tail_space > cap) {
            pr_debug(STR("recv_buf_write: invalid wrap length\n"));
            return result_error(EINVAL);
        }
        memcpy(buf->data.dat, data.dat + tail_space, data.len - tail_space);
    }

    buf->head = (head + data.len) & (cap - 1);
    return result_ok();
}

static sz recv_buf_read(struct recv_buf *buf, struct byte_buf *dest)
{
    /* Security: Always validate pointers in production */
    ALWAYS_ASSERT(buf != NULL);
    ALWAYS_ASSERT(dest != NULL);

    sz available = recv_buf_count(*buf);
    sz space = dest->cap - dest->len;

    /* Security: Validate destination buffer has valid capacity */
    if (dest->cap <= 0 || dest->len < 0) {
        pr_debug(STR("recv_buf_read: invalid destination buffer\n"));
        return 0;
    }

    /* Security: Prevent negative space calculation */
    if (space < 0) {
        pr_debug(STR("recv_buf_read: destination buffer overflow\n"));
        return 0;
    }

    sz to_read = MIN(available, space);

    if (to_read == 0)
        return 0;

    sz cap = buf->data.len;
    /* Security: Validate ring buffer capacity */
    if (cap <= 0 || !is_power_of_2(cap)) {
        pr_debug(STR("recv_buf_read: invalid buffer capacity\n"));
        return 0;
    }

    sz tail = buf->tail;
    /* Security: Validate tail pointer is within bounds */
    if (tail < 0 || tail >= cap) {
        pr_debug(STR("recv_buf_read: tail pointer out of bounds\n"));
        return 0;
    }

    sz tail_avail = cap - tail;

    /* Security: Bounds-checked memcpy for ring buffer read */
    if (to_read <= tail_avail) {
        /* Verify source and destination bounds */
        if (tail + to_read > cap || dest->len + to_read > dest->cap) {
            pr_debug(STR("recv_buf_read: read would overflow\n"));
            return 0;
        }
        memcpy(dest->dat + dest->len, buf->data.dat + tail, to_read);
    } else {
        /* Wrap case: validate first memcpy */
        if (tail_avail <= 0 || tail + tail_avail > cap) {
            pr_debug(STR("recv_buf_read: invalid tail_avail\n"));
            return 0;
        }
        if (dest->len + tail_avail > dest->cap) {
            pr_debug(
                STR("recv_buf_read: first wrap would "
                    "overflow\n"));
            return 0;
        }
        memcpy(dest->dat + dest->len, buf->data.dat + tail, tail_avail);

        /* Second wrap: validate remaining data */
        sz remaining = to_read - tail_avail;
        if (remaining <= 0 || remaining > cap) {
            pr_debug(
                STR("recv_buf_read: invalid remaining "
                    "length\n"));
            return 0;
        }
        if (dest->len + tail_avail + remaining > dest->cap) {
            pr_debug(
                STR("recv_buf_read: second wrap would "
                    "overflow\n"));
            return 0;
        }
        memcpy(dest->dat + dest->len + tail_avail, buf->data.dat, remaining);
    }

    buf->tail = (tail + to_read) & (cap - 1);
    dest->len += to_read;
    return to_read;
}

/* Send buffer queue */

#define TCP_SBQ_NUM 1024
#define TCP_SB_MAX_LEN 2048

static struct pool global_tcp_sbq_alloc;
static struct pool global_tcp_sb_alloc;

static sz global_tcp_stats_sbq_mem;
static u64 global_tcp_next_timer_ms = TIME_MS_MAX;

struct send_buf_queue {
    struct list_head link;
    struct send_buf sb;

    u32 seq_num, required_ack;
    u8 flags;
    net_u16 checksum;
    sz len;

    sz n_transmissions;
    struct time_ms retry_after, last_try;
};

static struct send_buf_queue *tcp_alloc_sbq_and_sb(void)
{
    struct send_buf_queue *sbq = pool_alloc(&global_tcp_sbq_alloc);
    if (!sbq)
        return NULL;

    void *sb_mem = pool_alloc(&global_tcp_sb_alloc);
    if (!sb_mem) {
        pool_free(&global_tcp_sbq_alloc, sbq);
        return NULL;
    }

    sbq->sb = send_buf_new(arena_new(byte_array_new(sb_mem, TCP_SB_MAX_LEN)));

    __atomic_fetch_add(&global_tcp_stats_sbq_mem,
                       global_tcp_sbq_alloc.size + global_tcp_sb_alloc.size,
                       __ATOMIC_RELAXED);

    return sbq;
}

static void tcp_free_sbq_and_sb(struct send_buf_queue *sbq)
{
    assert(sbq);

    pool_free(&global_tcp_sb_alloc, sbq->sb.orig_arn.beg);
    pool_free(&global_tcp_sbq_alloc, sbq);

    __atomic_fetch_sub(&global_tcp_stats_sbq_mem,
                       global_tcp_sbq_alloc.size + global_tcp_sb_alloc.size,
                       __ATOMIC_RELAXED);
}

/* TCP connection */

enum tcp_conn_state {
    /* Expect user to close connection after getting handle with 'listen' */
    TCP_CONN_STATE_LISTEN, /* Waiting for client to send SYN for connection */

    /* Expect user to close connection after getting handling with 'accept' */
    TCP_CONN_STATE_ESTABLISHED,
    TCP_CONN_STATE_CLOSE_WAIT,
    TCP_CONN_STATE_RESET, /* Special state that's not included in the normal TCP
                           * state transitions.
                           */

    /* Internal states, user has no access to connections in these states. */
    TCP_CONN_STATE_SYN_SENT,
    TCP_CONN_STATE_SYN_RCVD,
    TCP_CONN_STATE_LAST_ACK,
    TCP_CONN_STATE_FIN_WAIT_1,
    TCP_CONN_STATE_FIN_WAIT_2,
    TCP_CONN_STATE_CLOSING,
    TCP_CONN_STATE_TIME_WAIT,
};

#define TCP_CONN_DEFAULT_MSS 536 /* Based on RFC 9293 */
/* Kept low to allow reusing connections quickly. */
#define TCP_CONN_TIME_WAIT 1000
#define TCP_CONN_RTO_MIN 200   /* Minimum RTO of 200ms */
#define TCP_CONN_RTO_MAX 30000 /* Maximum RTO of 30s */
/* Connections in interal states time out after 30s */
#define TCP_CONN_INTERNAL_TIMEOUT 30000
/* Half-open connections expire after 4s to limit SYN flood impact */
#define TCP_CONN_SYN_RCVD_TIMEOUT 4000
/* Active-open SYN retry window (10s total before giving up) */
#define TCP_CONN_SYN_SENT_TIMEOUT 10000
/* RFC 1122 §4.2.3.6: idle time before first keep-alive probe */
#define TCP_CONN_KEEPALIVE_IDLE_MS 30000
/* Interval between successive keep-alive probes */
#define TCP_CONN_KEEPALIVE_INTVL_MS 10000
/* Drop connection after this many unanswered probes */
#define TCP_CONN_KEEPALIVE_CNT 8
/* Send zero-window probe after one RTO of zero window */
#define TCP_CONN_PERSIST_RTO_MS TCP_CONN_RTO_MIN
#define TCP_CONN_RECV_BUF_SIZE 0x4000
static_assert((TCP_CONN_RECV_BUF_SIZE & (TCP_CONN_RECV_BUF_SIZE - 1)) == 0,
              "TCP_CONN_RECV_BUF_SIZE must be power of 2 for bitwise ring "
              "buffer");
#define TCP_CONN_DEFAULT_RECV_WINDOW_SIZE (TCP_CONN_RECV_BUF_SIZE / 2)

/* Initial congestion window: 10 segments (RFC 6928). */
#define TCP_CONN_INITIAL_CWND 10
/* Fast retransmit threshold: 3 duplicate ACKs (RFC 5681 §3.2). */
#define TCP_CONN_DUPACK_THRESH 3

/* Congestion control vtable.
 *
 * Pluggable congestion control algorithm.  Each connection holds a pointer to
 * a const struct tcp_cc_ops.
 * Default: Reno (RFC 5681).
 * TODO: Implement Cubic and BBR.
 * All callbacks run under tcp_pool_lock (coarse lock).
 */
struct tcp_conn; /* forward */

struct tcp_cc_ops {
    const char *name;
    void (*on_ack)(struct tcp_conn *c, u32 bytes_acked);
    void (*on_dup_ack)(struct tcp_conn *c);
    void (*on_timeout)(struct tcp_conn *c);
};

/* Reno congestion control (RFC 5681). Implemented below after struct tcp_conn
 * is defined.
 */
static void tcp_cc_reno_on_ack(struct tcp_conn *c, u32 bytes_acked);
static void tcp_cc_reno_on_dup_ack(struct tcp_conn *c);
static void tcp_cc_reno_on_timeout(struct tcp_conn *c);

static const struct tcp_cc_ops tcp_cc_reno = {
    .name = "reno",
    .on_ack = tcp_cc_reno_on_ack,
    .on_dup_ack = tcp_cc_reno_on_dup_ack,
    .on_timeout = tcp_cc_reno_on_timeout,
};

#if CONFIG_TCP_SACK
struct tcp_sack_block {
    u32 left_edge;
    u32 right_edge;
};
#endif

struct tsopt {
    bool have_ts;
    u32 tsval, tsecr;
#if CONFIG_TCP_SACK
    bool have_sack_permitted;
    u8 n_sack_blocks;
    struct tcp_sack_block sack_blocks[3];
#endif
};

/* Magic number for struct tcp_conn (ASCII 'tcp '). */
#define TCP_MAGIC 0x74637020U

struct tcp_conn {
    sz mss;
    sz send_window_real;
    sz recv_window_size_real;
    struct time_ms internal_timeout;
    u64 last_activity_ms;
    u64 zero_window_since_ms;
    sz ooo_len;
    const struct tcp_cc_ops *cc;
    u64 stats_pkts_sent, stats_pkts_recv, stats_bytes_sent, stats_bytes_recv;
    const struct tcp_notify *notify;
    struct tcp_conn *listener;
    struct list_head accept_queue;
    struct list_head send_queue;
    struct list_head hash_node;
    struct recv_buf recv_buf;
    u32 magic;
    u32 generation;
    spinlock_t lock;
    enum tcp_conn_state state;
    u32 send_unack;
    u32 send_next;
    u32 iss;
    u32 recv_next;
    u32 ts_recent;
    u32 last_ack_sent;
    u32 srtt;
    u32 rttvar;
    u32 rto;
    u32 keepalive_probes;
    u32 ooo_seq_num;
    u32 cwnd;
    u32 ssthresh;
    u32 dup_ack_count;
    u32 stats_retransmits, stats_dup_acks, stats_ooo_segments;
    u16 host_port;
    u16 peer_port;
    bool is_used;
    bool use_window_scale;
    u8 send_window_scale;
    bool use_time_stamps;
    bool in_fast_recovery;
    /* recv_ready_bytes removed: use recv_buf_count() under tcp_pool_lock
     * for the authoritative byte count, or the lockless hint below for polls.
     */
    u32 accept_ready;
    struct ipv4_addr host_addr;
    struct ipv4_addr peer_addr;
    u8 ooo_buf[1500];
#if CONFIG_TCP_SACK
    u8 n_ooo_slots;
    struct tcp_sack_block ooo_blocks[3];
#endif
};

static inline const struct tcp_notify *tcp_notify_get(struct tcp_conn *conn)
{
    u64 flags = spin_lock_irqsave(&conn->lock);
    const struct tcp_notify *notify = conn->notify;
    spin_unlock_irqrestore(&conn->lock, flags);
    return notify;
}

/* Event notification dispatch. The notify pointer is snapshotted under
 * conn->lock so tcp_conn_set_notify() no longer contends on tcp_pool_lock.
 * Callbacks must NOT call back into any tcp_conn_* API.
 */
#define TCP_NOTIFY(c, cb, ...)                              \
    do {                                                    \
        const struct tcp_notify *_ntfy = tcp_notify_get(c); \
        if (_ntfy && _ntfy->cb)                             \
            _ntfy->cb((c), ##__VA_ARGS__);                  \
    } while (0)

/* Network operations vtable. TCP calls timing and IP functions through this
 * module-internal pointer. The default instance wires to real platform
 * functions; kernel selftests can swap in a mock.
 */
const struct net_ops net_ops_default = {
    .current_ms = time_current_ms,
    .rdtime = time_rdtime,
    .timebase_freq = time_get_timebase_freq,
    .ip_send = ipv4_send_packet,
    .ip_route_addr = ipv4_route_interface_addr,
    .ip_route_mtu = ipv4_route_mtu,
};
static const struct net_ops *tcp_net_ops = &net_ops_default;

static struct pool global_tcp_recv_buf_alloc;

/* To handle more connections simultaneously, this array could also be allocated
 * dynamically. The main reason for using array is that it is simple to search
 * (without requiring much pointer chasing like linked lists do).
 */
#define TCP_CONN_MAX_NUM 64
static struct tcp_conn global_tcp_conn_table[TCP_CONN_MAX_NUM];

static inline bool tcp_conn_needs_user_close(struct tcp_conn *conn);

/* TCP state transition helper with KTRACE emission.
 * tcp_conn_alloc_and_init() is exempt (freshly allocated slot).
 */
#ifdef CONFIG_EVENTLOG
static inline struct str ktrace_tcp_state(enum tcp_conn_state s)
{
    switch (s) {
    case TCP_CONN_STATE_LISTEN:
        return STR("LISTEN");
    case TCP_CONN_STATE_ESTABLISHED:
        return STR("ESTABLISHED");
    case TCP_CONN_STATE_CLOSE_WAIT:
        return STR("CLOSE_WAIT");
    case TCP_CONN_STATE_RESET:
        return STR("RESET");
    case TCP_CONN_STATE_SYN_SENT:
        return STR("SYN_SENT");
    case TCP_CONN_STATE_SYN_RCVD:
        return STR("SYN_RCVD");
    case TCP_CONN_STATE_LAST_ACK:
        return STR("LAST_ACK");
    case TCP_CONN_STATE_FIN_WAIT_1:
        return STR("FIN_WAIT_1");
    case TCP_CONN_STATE_FIN_WAIT_2:
        return STR("FIN_WAIT_2");
    case TCP_CONN_STATE_CLOSING:
        return STR("CLOSING");
    case TCP_CONN_STATE_TIME_WAIT:
        return STR("TIME_WAIT");
    default:
        return STR("UNKNOWN");
    }
}
#endif

static inline void tcp_set_state(struct tcp_conn *conn,
                                 enum tcp_conn_state new_state)
{
#ifdef CONFIG_EVENTLOG
    assert(conn >= global_tcp_conn_table &&
           conn < global_tcp_conn_table + TCP_CONN_MAX_NUM);
    u16 idx = (u16) (conn - global_tcp_conn_table);
    KTRACE("event=tcp_state_changed conn=%hu old=%s new=%s", (u32) idx,
           ktrace_tcp_state(conn->state), ktrace_tcp_state(new_state));
#endif
    conn->state = new_state;
}

/* Insert a connection into the hash table by its 4-tuple.
 * Only called for connections with a known peer (not listen sockets).
 * Caller must hold tcp_pool_lock.
 */
static void tcp_hash_insert(struct tcp_conn *conn)
{
    /* Guard against double-insert: node must be self-referential. */
    assert(conn->hash_node.next == &conn->hash_node);
    u32 bucket = tcp_hash_4tuple(conn->host_addr, conn->host_port,
                                 conn->peer_addr, conn->peer_port) &
                 TCP_HASH_MASK;
    list_add(&tcp_hash_table[bucket], &conn->hash_node);
}

/* Remove a connection from the hash table. Safe to call even if never inserted
 * (list_del_init on a self-referential node is a no-op).
 */
static void tcp_hash_remove(struct tcp_conn *conn)
{
    list_del_init(&conn->hash_node);
}

/* Ephemeral port allocator for outbound connections. */
static u16 tcp_ephemeral_port_next = 49152;

static u16 tcp_alloc_ephemeral_port(void)
{
    u16 start = tcp_ephemeral_port_next;
    for (;;) {
        u16 port = tcp_ephemeral_port_next++;
        if (tcp_ephemeral_port_next < 49152)
            tcp_ephemeral_port_next = 49152;

        /* Check no existing connection uses this port. */
        bool in_use = false;
        for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
            if (global_tcp_conn_table[i].is_used &&
                global_tcp_conn_table[i].host_port == port) {
                in_use = true;
                break;
            }
        }
        if (!in_use)
            return port;

        /* Wrapped around without finding a free port. */
        if (tcp_ephemeral_port_next == start)
            return 0;
    }
}

static void tcp_free_conn(struct tcp_conn *conn)
{
    assert(conn);
    struct send_buf_queue *sbq;
    MAGIC_CHECK(conn, TCP_MAGIC);

#ifdef CONFIG_EVENTLOG
    /* Emit terminal CLOSED transition before the struct is poisoned. There is
     * no TCP_CONN_STATE_CLOSED enum - connections go straight from their final
     * state to slot reuse - so emit the event directly.
     */
    {
        u16 idx = (u16) (conn - global_tcp_conn_table);
        KTRACE("event=tcp_state_changed conn=%hu old=%s new=%s", (u32) idx,
               ktrace_tcp_state(conn->state), STR("CLOSED"));
    }
#endif

    if (conn->listener && conn->accept_queue.next != &conn->accept_queue &&
        tcp_conn_needs_user_close(conn)) {
        u64 listener_flags = spin_lock_irqsave(&conn->listener->lock);
        if (conn->listener->accept_ready > 0)
            conn->listener->accept_ready--;
        spin_unlock_irqrestore(&conn->listener->lock, listener_flags);
    }

    /* Fire on_disconnect only if not already fired at RST/FIN transition.
     * States RESET and CLOSE_WAIT already dispatched on_disconnect when the
     * state change occurred; firing again would be a spurious duplicate.
     */
    if (conn->state != TCP_CONN_STATE_RESET &&
        conn->state != TCP_CONN_STATE_CLOSE_WAIT)
        TCP_NOTIFY(conn, on_disconnect);
    u64 conn_flags = spin_lock_irqsave(&conn->lock);
    conn->notify = NULL;
    spin_unlock_irqrestore(&conn->lock, conn_flags);

    tcp_hash_remove(conn);
    recv_buf_free(&conn->recv_buf, &global_tcp_recv_buf_alloc);

    list_for_each_entry_safe (&conn->send_queue, sbq, struct send_buf_queue,
                              link) {
        list_del(&sbq->link);
        tcp_free_sbq_and_sb(sbq);
    }

    list_del_init(&conn->accept_queue);

    /* Increment generation before poisoning - stale references from this
     * generation will fail the check after the slot is reallocated.
     */
    u32 gen = conn->generation + 1;

    /* Poison the struct so stale accesses are recognizable in memory dumps. */
    byte_array_set(byte_array_new((void *) conn, sizeof(*conn)), 0xee);
    conn->generation = gen;
    conn->is_used = false;
    __atomic_fetch_sub(&global_tcp_stats_n_connections, 1, __ATOMIC_RELAXED);
}

static inline bool tcp_conn_needs_user_close(struct tcp_conn *conn)
{
    /* User-visible connections remain owned by the caller until close().
     * That includes SYN_SENT, ESTABLISHED, CLOSE_WAIT, and RESET.
     */
    return conn->state == TCP_CONN_STATE_SYN_SENT ||
           conn->state == TCP_CONN_STATE_ESTABLISHED ||
           conn->state == TCP_CONN_STATE_CLOSE_WAIT ||
           conn->state == TCP_CONN_STATE_RESET;
}

static inline bool tcp_conn_is_internal(struct tcp_conn *conn)
{
    return conn->state == TCP_CONN_STATE_SYN_RCVD ||
           conn->state == TCP_CONN_STATE_LAST_ACK ||
           conn->state == TCP_CONN_STATE_FIN_WAIT_1 ||
           conn->state == TCP_CONN_STATE_FIN_WAIT_2 ||
           conn->state == TCP_CONN_STATE_CLOSING ||
           conn->state == TCP_CONN_STATE_TIME_WAIT;
}

/* Nudge the cached next-timer if 'deadline_ms' is earlier.  Lock-free:
 * the worst case is a stale read that delays discovery by one poll cycle,
 * which is bounded by the caller's retransmit/keepalive interval.
 */
static inline void tcp_timer_hint(u64 deadline_ms)
{
    u64 cur = __atomic_load_n(&global_tcp_next_timer_ms, __ATOMIC_RELAXED);
    while (deadline_ms < cur) {
        if (__atomic_compare_exchange_n(&global_tcp_next_timer_ms, &cur,
                                        deadline_ms, true, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED))
            break;
    }
}

static inline void tcp_conn_set_internal_timeout(struct tcp_conn *conn)
{
    assert(tcp_conn_is_internal(conn));

    u64 now = tcp_net_ops->current_ms().ms;
    if (conn->state == TCP_CONN_STATE_TIME_WAIT)
        conn->internal_timeout.ms = now + TCP_CONN_TIME_WAIT;
    else if (conn->state == TCP_CONN_STATE_SYN_RCVD)
        conn->internal_timeout.ms = now + TCP_CONN_SYN_RCVD_TIMEOUT;
    else
        conn->internal_timeout.ms = now + TCP_CONN_INTERNAL_TIMEOUT;

    tcp_timer_hint(conn->internal_timeout.ms);
}

static inline void tcp_purge_old_conn(void)
{
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *conn = &global_tcp_conn_table[i];
        if (!conn->is_used)
            continue;

        if (tcp_conn_is_internal(conn) &&
            tcp_net_ops->current_ms().ms >= conn->internal_timeout.ms) {
            tcp_free_conn(conn);
            continue;
        }

        /* SYN_SENT timeout: user holds pointer, so transition to RESET rather
         * than freeing. User detects this via tcp_conn_is_connected returning
         * false and must call tcp_conn_close().
         */
        if (conn->state == TCP_CONN_STATE_SYN_SENT &&
            tcp_net_ops->current_ms().ms >= conn->internal_timeout.ms) {
            tcp_set_state(conn, TCP_CONN_STATE_RESET);
        }
    }
}

static struct tcp_conn *tcp_alloc_conn(void)
{
    tcp_purge_old_conn();

    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *conn = &global_tcp_conn_table[i];

        if (!conn->is_used) {
            conn->magic = TCP_MAGIC;
            conn->is_used = true;
            conn->lock = (spinlock_t) SPINLOCK_INITIALIZER;
            conn->notify = NULL;
            list_init(&conn->hash_node);
            __atomic_fetch_add(&global_tcp_stats_n_connections, 1,
                               __ATOMIC_RELAXED);
            return conn;
        }
    }

    return NULL;
}

static struct tcp_conn *tcp_lookup_conn(struct ipv4_addr host_addr,
                                        struct ipv4_addr peer_addr,
                                        u16 host_port,
                                        u16 peer_port)
{
    /* Fast path: exact-match via hash table (O(1) amortized). Connected sockets
     * have known peer addresses and are hashed by their full 4-tuple.
     */
    u32 bucket = tcp_hash_4tuple(host_addr, host_port, peer_addr, peer_port) &
                 TCP_HASH_MASK;
    struct tcp_conn *conn;
    list_for_each_entry_safe (&tcp_hash_table[bucket], conn, struct tcp_conn,
                              hash_node) {
        if (ipv4_addr_is_equal(host_addr, conn->host_addr) &&
            host_port == conn->host_port &&
            ipv4_addr_is_equal(peer_addr, conn->peer_addr) &&
            peer_port == conn->peer_port)
            return conn;
    }

    /* Slow path: linear scan for wildcard/listen sockets (typically 1).
     * Listen sockets have peer 0.0.0.0:0 and cannot be hashed.
     * Wildcard match: 0.0.0.0 or port 0 matches any address/port.
     */
    struct ipv4_addr zero = IPV4_ADDR_ANY;
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        conn = &global_tcp_conn_table[i];
        if (!conn->is_used)
            continue;

        if (!ipv4_addr_is_equal(host_addr, conn->host_addr) ||
            host_port != conn->host_port)
            continue;

        bool peer_match = ipv4_addr_is_equal(peer_addr, zero) ||
                          ipv4_addr_is_equal(conn->peer_addr, zero) ||
                          ipv4_addr_is_equal(peer_addr, conn->peer_addr);
        bool port_match = peer_port == 0 || conn->peer_port == 0 ||
                          peer_port == conn->peer_port;
        if (peer_match && port_match)
            return conn;
    }

    return NULL;
}

static struct str tcp_conn_format_raw(struct ipv4_addr host_addr,
                                      struct ipv4_addr peer_addr,
                                      u16 host_port,
                                      u16 peer_port,
                                      struct arena *arn)
{
    assert(arn);
    struct str_buf sbuf =
        str_buf_from_byte_array(byte_array_from_arena(128, arn));
    assert(!fmt(&sbuf, STR("%s:%hu %s:%hu"), ipv4_addr_format(host_addr, arn),
                host_port, ipv4_addr_format(peer_addr, arn), peer_port)
                .is_error);
    return str_from_buf(sbuf);
}

struct str tcp_conn_format(struct tcp_conn *conn, struct arena *arn)
{
    assert(conn);
    return tcp_conn_format_raw(conn->host_addr, conn->peer_addr,
                               conn->host_port, conn->peer_port, arn);
}

static struct result_u32 tcp_generate_isn(void)
{
    u64 isn_raw;
    bool success = pseudo_rand_u64(&isn_raw);
    if (!success)
        return result_u32_error(EIO);
    return result_u32_ok((u32) isn_raw);
}

static struct tcp_conn *tcp_conn_alloc_and_init(struct ipv4_addr host_addr,
                                                u16 host_port,
                                                sz mss,
                                                enum tcp_conn_state state)
{
    /* Temporary arena for formatting IP address in error messages */
    byte addr_fmt_buf[IP_ADDR_FMT_BUF_SIZE];
    struct arena addr_fmt_arn =
        arena_new(byte_array_new(addr_fmt_buf, sizeof(addr_fmt_buf)));

    struct tcp_conn *conn = tcp_alloc_conn();
    if (!conn) {
        pr_debug(STR("TCP: Failed to allocate connection for %s:%hu\n"),
                 ipv4_addr_format(host_addr, &addr_fmt_arn), host_port);
        return NULL;
    }

    struct result_u32 isn_res = tcp_generate_isn();
    if (isn_res.is_error) {
        pr_debug(STR("TCP: Failed to generate ISN for %s:%hu: err=%d\n"),
                 ipv4_addr_format(host_addr, &addr_fmt_arn), host_port,
                 isn_res.code);
        tcp_free_conn(conn);
        return NULL;
    }

    conn->host_addr = host_addr;
    conn->peer_addr = IPV4_ADDR_ANY;
    conn->host_port = host_port;
    conn->peer_port = 0;
    conn->state = state;
    conn->listener = NULL;

    conn->mss = mss;
    conn->use_window_scale = false;

    list_init(&conn->accept_queue);

    conn->recv_next = 0;
    conn->recv_buf.data = byte_array_new(NULL, 0);
    conn->recv_buf.head = 0;
    conn->recv_buf.tail = 0;
    conn->recv_window_size_real = TCP_CONN_DEFAULT_RECV_WINDOW_SIZE;

    conn->iss = result_u32_checked(isn_res);
    conn->send_unack = conn->iss;
    conn->send_next = conn->iss;
    conn->send_window_real = 0;
    conn->send_window_scale = 0;

    list_init(&conn->send_queue);

    conn->use_time_stamps = false;
    conn->ts_recent = 0;
    conn->last_ack_sent = 0;

    conn->srtt = 0;
    conn->rttvar = 0;
    conn->rto = TCP_CONN_RTO_MIN;

    /* This way, timer cannot expire by default because actual time will never
     * reach 'TIME_MS_MAX'.
     */
    conn->internal_timeout = time_ms_new(TIME_MS_MAX);

    conn->last_activity_ms = tcp_net_ops->current_ms().ms;
    conn->keepalive_probes = 0;
    conn->zero_window_since_ms = 0;
    tcp_timer_hint(conn->last_activity_ms + (u64) TCP_CONN_KEEPALIVE_IDLE_MS);
    conn->ooo_len = 0;

    /* Congestion control: Reno by default.  IW = 10 * MSS (RFC 6928). */
    conn->cc = &tcp_cc_reno;
    conn->cwnd = TCP_CONN_INITIAL_CWND * conn->mss;
    conn->ssthresh = 0xFFFFFFFFU; /* effectively infinite until first loss */
    conn->dup_ack_count = 0;
    conn->in_fast_recovery = false;
    /* (recv_ready_bytes removed — recv_buf_count is authoritative) */
    conn->accept_ready = 0;

    conn->stats_pkts_sent = 0;
    conn->stats_pkts_recv = 0;
    conn->stats_bytes_sent = 0;
    conn->stats_bytes_recv = 0;
    conn->stats_retransmits = 0;
    conn->stats_dup_acks = 0;
    conn->stats_ooo_segments = 0;

    return conn;
}

static u32 tcp_get_timestamp_clock(void)
{
    return (u32) tcp_net_ops->current_ms().ms;
}

static inline sz seq_num_relative(struct tcp_conn *conn, sz seq_num)
{
    return (u32) seq_num - (u32) conn->iss;
}

/* Wrapping-safe TCP sequence number comparisons (RFC 793 §3.3).
 * Sequence numbers are unsigned 32-bit integers that wrap past 0xFFFFFFFF.
 * Signed comparison of the difference handles the wraparound correctly:
 * seq_lt(0xFFFFFFFF, 0x00000001) is true because (i32)(0xFFFFFFFF - 1) == -2.
 */
static inline bool seq_lt(u32 a, u32 b)
{
    return (i32) (a - b) < 0;
}

static inline bool seq_le(u32 a, u32 b)
{
    return (i32) (a - b) <= 0;
}

static inline bool seq_gt(u32 a, u32 b)
{
    return (i32) (a - b) > 0;
}

static inline bool seq_geq(u32 a, u32 b)
{
    return (i32) (a - b) >= 0;
}

static inline u32 abs_diff(u32 a, u32 b)
{
    return (a > b) ? (a - b) : (b - a);
}

static void tcp_conn_update_rtt(struct tcp_conn *conn, u32 rtt_sample)
{
    /* This is according to RFC 6298. */

    if (conn->srtt == 0) {
        conn->srtt = rtt_sample;
        conn->rttvar = rtt_sample / 2;
    } else {
        conn->rttvar = (3 * conn->rttvar + abs_diff(rtt_sample, conn->srtt)) /
                       4;                               /* beta = 1/4 */
        conn->srtt = (7 * conn->srtt + rtt_sample) / 8; /* alpha = 1/8 */
    }

    conn->rto = conn->srtt + MAX(50, 4 * conn->rttvar); /* Minimum granularity
                                                         * of 50ms.
                                                         */
    conn->rto = MIN(TCP_CONN_RTO_MAX, MAX(TCP_CONN_RTO_MIN, conn->rto));
}

/* Reno congestion control (RFC 5681).
 *
 * Slow start: cwnd += MSS per ACK (exponential growth) while cwnd < ssthresh.
 * Congestion avoidance: cwnd += MSS * MSS / cwnd per ACK (linear growth).
 * Fast recovery entry (3 dup-ACKs): ssthresh = max(flight/2, 2*MSS),
 * cwnd = ssthresh + 3*MSS, in_fast_recovery = true.
 * Fast recovery inflation (4th+ dup-ACK): cwnd += MSS.
 * Fast recovery exit (new ACK): cwnd = ssthresh (deflate), exit recovery.
 * RTO: ssthresh = max(flight/2, 2*MSS), cwnd = 1*MSS.
 *
 * cwnd is capped at 0x7FFFFFFFU to prevent u32 overflow on long-lived flows.
 */

#define TCP_CWND_MAX 0x7FFFFFFFU

static inline u32 tcp_cc_mss(struct tcp_conn *c)
{
    return (c->mss > 0) ? (u32) c->mss : TCP_CONN_DEFAULT_MSS;
}

static void tcp_cc_reno_on_ack(struct tcp_conn *c, u32 bytes_acked)
{
    u32 mss = tcp_cc_mss(c);

    /* Exit fast recovery: deflate cwnd to ssthresh (RFC 5681 §3.2 step 3). */
    if (c->in_fast_recovery) {
        c->cwnd = c->ssthresh;
        c->in_fast_recovery = false;
    }

    if (c->cwnd < c->ssthresh) {
        /* Slow start: increase cwnd by the number of bytes ACKed, capped at MSS
         * (one segment per ACK).
         */
        u32 inc = MIN(bytes_acked, mss);
        c->cwnd = (c->cwnd + inc > TCP_CWND_MAX) ? TCP_CWND_MAX : c->cwnd + inc;
    } else {
        /* Congestion avoidance: approximately one MSS per RTT.
         * cwnd += MSS * (bytes_acked / cwnd), with minimum 1 byte.
         */
        u32 inc = (u32) ((u64) mss * bytes_acked / c->cwnd);
        if (inc == 0)
            inc = 1;
        c->cwnd = (c->cwnd + inc > TCP_CWND_MAX) ? TCP_CWND_MAX : c->cwnd + inc;
    }
}

static void tcp_cc_reno_on_dup_ack(struct tcp_conn *c)
{
    u32 mss = tcp_cc_mss(c);

    if (c->dup_ack_count == TCP_CONN_DUPACK_THRESH) {
        /* Enter fast recovery (RFC 5681 §3.2 step 1-2). */
        u32 in_flight = c->send_next - c->send_unack;
        c->ssthresh = MAX(in_flight / 2, 2 * mss);
        c->cwnd = c->ssthresh + 3 * mss;
        c->in_fast_recovery = true;
    } else if (c->dup_ack_count > TCP_CONN_DUPACK_THRESH &&
               c->in_fast_recovery) {
        /* Window inflation: each additional dup-ACK means one segment has left
         * the network (RFC 5681 §3.2 step 4).
         */
        c->cwnd = (c->cwnd + mss > TCP_CWND_MAX) ? TCP_CWND_MAX : c->cwnd + mss;
    }
}

static void tcp_cc_reno_on_timeout(struct tcp_conn *c)
{
    u32 mss = tcp_cc_mss(c);

    u32 in_flight = c->send_next - c->send_unack;
    c->ssthresh = MAX(in_flight / 2, 2 * mss);
    c->cwnd = mss; /* RFC 5681 §3.1: reset to one segment */
    c->dup_ack_count = 0;
    c->in_fast_recovery = false;
}

static void tcp_conn_update_send_state(struct tcp_conn *conn,
                                       struct tcp_header *hdr,
                                       struct tsopt tsopt)
{
    assert(conn);

    if (hdr->flags & TCP_HDR_FLAG_ACK) {
        u32 ack_num = u32_from_net_u32(hdr->ack_num);

        /* RFC 793: ignore ACKs for data not yet sent. */
        if (seq_gt(ack_num, conn->send_unack) &&
            seq_le(ack_num, conn->send_next)) {
            u32 bytes_acked = ack_num - conn->send_unack;
            conn->send_unack = ack_num;

            /* New data acknowledged: reset dup-ACK counter, notify CC. */
            conn->dup_ack_count = 0;
            if (conn->cc && conn->cc->on_ack)
                conn->cc->on_ack(conn, bytes_acked);

            if (conn->use_time_stamps && tsopt.have_ts && tsopt.tsecr != 0) {
                u32 current_time = tcp_get_timestamp_clock();
                u32 rtt_sample = current_time - tsopt.tsecr;
                /* Discard samples > 60s (clock wrap or stale echoed TS). */
                if (rtt_sample <= 60000)
                    tcp_conn_update_rtt(conn, rtt_sample);
            }
        } else if (ack_num == conn->send_unack &&
                   conn->send_unack != conn->send_next) {
            /* Duplicate ACK: same ack_num, no new data ACKed, with outstanding
             * data in flight (RFC 5681 §3.2).
             */
            conn->dup_ack_count++;
            conn->stats_dup_acks++;
            if (conn->cc && conn->cc->on_dup_ack)
                conn->cc->on_dup_ack(conn);
        }
    }

    /* send_window_scale stays at 0 when window scaling was not negotiated. */
    sz prev_window = conn->send_window_real;
    conn->send_window_real = u16_from_net_u16(hdr->window_size)
                             << conn->send_window_scale;

    if (conn->send_window_real != prev_window)
        TCP_NOTIFY(conn, on_window_update, (u32) conn->send_window_real);

    if (conn->send_window_real > 0 && prev_window == 0) {
        /* Window just opened: cancel any pending persist probe. */
        conn->zero_window_since_ms = 0;
    } else if (conn->send_window_real == 0 && prev_window > 0) {
        /* Window just closed: start persist timer. */
        u64 now = tcp_net_ops->current_ms().ms;
        conn->zero_window_since_ms = now;
        tcp_timer_hint(now + (u64) TCP_CONN_PERSIST_RTO_MS);
    }
}

static sz tcp_conn_update_recv_state(struct tcp_conn *conn,
                                     struct tcp_header *hdr,
                                     struct tsopt tsopt,
                                     struct byte_view payload,
                                     struct arena tmp)
{
    assert(conn);
    assert(hdr);

    u32 seq_num = u32_from_net_u32(hdr->seq_num);

    if (conn->use_time_stamps && tsopt.have_ts &&
        seq_geq(conn->last_ack_sent, seq_num) &&
        seq_gt(seq_num + (u32) payload.len, conn->last_ack_sent))
        conn->ts_recent = tsopt.tsval;

    if (payload.len > 0) {
        if (seq_num == conn->recv_next) {
            /* In-order segment: deliver to the receive buffer. */
            struct result write_res = recv_buf_write(&conn->recv_buf, payload);
            if (write_res.is_error) {
                assert(write_res.code == EAGAIN);
                pr_warn(STR("Not enough space in receive buffer to receive "
                            "incoming segment (%s). Dropping ...\n"),
                        tcp_conn_format(conn, &tmp));
                return 0;
            }
            conn->recv_next += payload.len;
            conn->recv_window_size_real -=
                MIN(conn->recv_window_size_real, (sz) payload.len);
            TCP_NOTIFY(conn, on_data_available);

            /* Flush or invalidate single OOO slot after advancing recv_next. */
            if (conn->ooo_len > 0) {
                if (conn->ooo_seq_num == conn->recv_next) {
                    /* OOO slot aligns; flush it into the receive buffer. */
                    struct byte_view ooo =
                        byte_view_new(conn->ooo_buf, conn->ooo_len);
                    struct result flush_res =
                        recv_buf_write(&conn->recv_buf, ooo);
                    if (!flush_res.is_error) {
                        conn->recv_next += conn->ooo_len;
                        conn->recv_window_size_real -= MIN(
                            conn->recv_window_size_real, (sz) conn->ooo_len);
                    }
                    conn->ooo_len = 0;
                } else if (seq_geq(conn->recv_next,
                                   conn->ooo_seq_num + (u32) conn->ooo_len)) {
                    /* A retransmission already covered the OOO data - discard
                     * the stale slot.
                     */
                    conn->ooo_len = 0;
                }
            }
        } else if (seq_gt(seq_num, conn->recv_next) && conn->ooo_len == 0 &&
                   payload.len <= (sz) sizeof(conn->ooo_buf) &&
                   !(hdr->flags & TCP_HDR_FLAG_FIN)) {
            /* One-gap out-of-order data segment: stash in the single OOO slot.
             * FIN segments are excluded; the peer will retransmit in order.
             */
            for (sz i = 0; i < payload.len; i++)
                conn->ooo_buf[i] = payload.dat[i];
            conn->ooo_seq_num = seq_num;
            conn->ooo_len = payload.len;
            conn->stats_ooo_segments++;
            pr_debug(STR("Buffered OOO segment seq=%u len=%zu (%s)\n"), seq_num,
                     (u64) payload.len, tcp_conn_format(conn, &tmp));
            return 0;
        } else {
            pr_debug(STR("Out-of-order segment received: expected seq=%u, got "
                         "seq=%u (%s). Dropping ...\n"),
                     conn->recv_next, seq_num, tcp_conn_format(conn, &tmp));
            return 0;
        }
    }

    if (hdr->flags & TCP_HDR_FLAG_FIN) {
        if (seq_num + payload.len != conn->recv_next) {
            pr_debug(STR("FIN received with unexpected sequence number (%s). "
                         "Dropping ...\n"),
                     tcp_conn_format(conn, &tmp));
            return payload.len;
        }

        conn->recv_next++; /* The incoming FIN consumed one sequence number. */
        if (conn->recv_window_size_real > 0)
            conn->recv_window_size_real--;
    }

    return payload.len;
}

/* TCP initialization */

struct result tcp_init(void)
{
    assert(!global_tcp_is_initialized);

    /* Initialize hash table buckets and seed from rdtime() low bits. */
    tcp_hash_seed = (u32) tcp_net_ops->rdtime();
    for (sz i = 0; i < TCP_HASH_BUCKETS; i++)
        list_init(&tcp_hash_table[i]);

    struct option_byte_array recv_mem_opt = kvalloc_alloc(
        (sz) TCP_CONN_RECV_BUF_SIZE * TCP_CONN_MAX_NUM, alignof(void *));
    if (recv_mem_opt.is_none)
        return result_error(ENOMEM);
    struct byte_array recv_mem = option_byte_array_checked(recv_mem_opt);
    global_tcp_recv_buf_alloc = pool_new(recv_mem, TCP_CONN_RECV_BUF_SIZE);

    struct option_byte_array sbq_mem_opt =
        kvalloc_alloc(sizeof(struct send_buf_queue) * TCP_SBQ_NUM,
                      alignof(struct send_buf_queue));
    if (sbq_mem_opt.is_none) {
        kvalloc_free(recv_mem);
        return result_error(ENOMEM);
    }
    struct byte_array sbq_mem = option_byte_array_checked(sbq_mem_opt);
    global_tcp_sbq_alloc = pool_new(sbq_mem, sizeof(struct send_buf_queue));

    struct option_byte_array sb_mem_opt =
        kvalloc_alloc((sz) TCP_SB_MAX_LEN * TCP_SBQ_NUM, alignof(void *));
    if (sb_mem_opt.is_none) {
        kvalloc_free(recv_mem);
        kvalloc_free(sbq_mem);
        return result_error(ENOMEM);
    }
    struct byte_array sb_mem = option_byte_array_checked(sb_mem_opt);
    global_tcp_sb_alloc = pool_new(sb_mem, TCP_SB_MAX_LEN);

    global_tcp_is_initialized = true;
    return result_ok();
}

static void tcp_init_hook(u32 lifecycle_flag __unused)
{
    assert(!tcp_init().is_error);
}
INIT_TASK("tcp",
          tcp_init_hook,
          INIT_REQUIRES(INITGRAPH_STAGE_SCHED),
          INIT_ENTAILS(INITGRAPH_STAGE_NET),
          INIT_FLAG_PRIMARY);

/* Transmit and retransmission logic */

static struct result tcp_send_segment_noqueue_raw(struct ipv4_addr host_addr,
                                                  struct ipv4_addr peer_addr,
                                                  u16 host_port,
                                                  u16 peer_port,
                                                  u32 seq_num,
                                                  u32 ack_num,
                                                  sz window_size_real,
                                                  u8 window_scale,
                                                  bool use_window_scale,
                                                  u8 flags,
                                                  net_u16 payload_checksum,
                                                  sz payload_len,
                                                  u32 ts_recent,
                                                  bool use_time_stamps,
                                                  struct send_buf sb,
                                                  struct arena tmp)
{
    /* Verify the connection's source address still matches the routed
     * interface. If it changed (DHCP renewal / reconfiguration), force RST so
     * the peer tears down cleanly instead of keeping a half-open session.
     */
    struct result_ipv4_addr iface_res = ipv4_route_interface_addr(peer_addr);
    if (iface_res.is_error)
        return result_error(iface_res.code);
    if (!ipv4_addr_is_equal(result_ipv4_addr_checked(iface_res), host_addr)) {
        pr_warn(STR("TCP: interface addr mismatch, forcing RST\n"));
        /* Strip SYN to avoid emitting SYN|RST with MSS/WS options —
         * peers commonly ignore malformed resets.
         */
        flags = TCP_HDR_FLAG_RST | TCP_HDR_FLAG_ACK;
    }

    bool use_mss = flags & TCP_HDR_FLAG_SYN;
    bool use_ws = (flags & TCP_HDR_FLAG_SYN) && use_window_scale;
    bool use_ts = use_time_stamps;

    struct tcp_option_mss mss;
    if (use_mss) {
        mss.kind = TCP_OPT_MSS_KIND;
        mss.length = TCP_OPT_MSS_LENGTH;
        mss.value = net_u16_from_u16(TCP_OPT_MSS_VALUE);
    }

    struct tcp_option_ws ws;
    if (use_ws) {
        ws.kind = TCP_OPT_WS_KIND;
        ws.length = TCP_OPT_WS_LENGTH;
        ws.value = window_scale;
        ws.nop = TCP_OPT_NOP_KIND;
    }

    struct tcp_option_ts ts;
    if (use_ts) {
        ts.kind = TCP_OPT_TS_KIND;
        ts.length = TCP_OPT_TS_LENGTH;
        ts.tsval = net_u32_from_u32(tcp_get_timestamp_clock());
        ts.tsecr = net_u32_from_u32(ts_recent);
        ts.nop1 = TCP_OPT_NOP_KIND;
        ts.nop2 = TCP_OPT_NOP_KIND;
    }

    sz opt_size = (use_mss ? sizeof(mss) : 0) + (use_ws ? sizeof(ws) : 0) +
                  (use_ts ? sizeof(ts) : 0);
    /* opt_size must be evenly divisible by 4. */
    assert(IS_ALIGNED(opt_size, 4));

    struct tcp_header hdr = {
        .src_port = net_u16_from_u16(host_port),
        .dest_port = net_u16_from_u16(peer_port),
        .seq_num = net_u32_from_u32(seq_num),
        .ack_num = net_u32_from_u32(ack_num),
        .header_len = TCP_HDR_LEN_NO_OPT + (opt_size / 4),
        .reserved = 0,
        .flags = flags,
        .window_size = net_u16_from_u16(window_size_real >> window_scale),
        .checksum = net_u16_from_u16(0),
        .urgent = net_u16_from_u16(0),
    };

    struct tcp_ip_pseudo_header pseudo_hdr = {
        .src_addr = host_addr,
        .dest_addr = peer_addr,
        .zero = 0,
        .protocol = IPV4_PROTOCOL_TCP,
        .tcp_length = net_u16_from_u16(sizeof(hdr) + opt_size + payload_len),
    };

    net_u16 checksum = net_u16_from_u16(0);
    checksum = internet_checksum_add(checksum, payload_checksum);
    checksum = internet_checksum_iterate(
        checksum, byte_view_new((void *) &hdr, sizeof(hdr)));
    checksum = internet_checksum_iterate(
        checksum, byte_view_new((void *) &pseudo_hdr, sizeof(pseudo_hdr)));
    if (use_mss)
        checksum = internet_checksum_iterate(
            checksum, byte_view_new((void *) &mss, sizeof(mss)));
    if (use_ws)
        checksum = internet_checksum_iterate(
            checksum, byte_view_new((void *) &ws, sizeof(ws)));
    if (use_ts)
        checksum = internet_checksum_iterate(
            checksum, byte_view_new((void *) &ts, sizeof(ts)));
    hdr.checksum = internet_checksum_finalize(checksum);

    struct byte_buf *buf = NULL;

    if (opt_size) {
        buf = send_buf_prepend(&sb, opt_size);
        if (!buf)
            return result_error(ENOMEM);
        if (use_mss)
            assert(byte_buf_append(buf,
                                   byte_view_new((void *) &mss, sizeof(mss))) ==
                   sizeof(mss));
        if (use_ws)
            assert(
                byte_buf_append(buf, byte_view_new((void *) &ws, sizeof(ws))) ==
                sizeof(ws));
        if (use_ts)
            assert(
                byte_buf_append(buf, byte_view_new((void *) &ts, sizeof(ts))) ==
                sizeof(ts));
    }

    buf = send_buf_prepend(&sb, sizeof(hdr));
    if (!buf)
        return result_error(ENOMEM);
    assert(byte_buf_append(buf, byte_view_new((void *) &hdr, sizeof(hdr))) ==
           sizeof(hdr));

    return ipv4_send_packet_from(host_addr, peer_addr, IPV4_PROTOCOL_TCP, sb,
                                 tmp);
}

static struct result tcp_send_segment_noqueue(struct tcp_conn *conn,
                                              u8 flags,
                                              u32 seq_num,
                                              net_u16 payload_checksum,
                                              sz payload_len,
                                              struct send_buf sb,
                                              struct arena arn)
{
    assert(conn);

    if (conn->state != TCP_CONN_STATE_LISTEN &&
        conn->state != TCP_CONN_STATE_SYN_SENT &&
        conn->state != TCP_CONN_STATE_SYN_RCVD) {
        sz space = recv_buf_space(conn->recv_buf);
        if (space - conn->recv_window_size_real >= TCP_OPT_MSS_VALUE)
            conn->recv_window_size_real = space;
    }

    if (flags & TCP_HDR_FLAG_ACK)
        conn->last_ack_sent = conn->recv_next;

    return tcp_send_segment_noqueue_raw(
        conn->host_addr, conn->peer_addr, conn->host_port, conn->peer_port,
        seq_num, conn->recv_next, conn->recv_window_size_real, 0,
        conn->use_window_scale, flags, payload_checksum, payload_len,
        conn->ts_recent, conn->use_time_stamps, sb, arn);
}

static struct result tcp_poll_retransmit_conn(struct tcp_conn *conn,
                                              struct arena tmp)
{
    assert(conn);
    assert(conn->state != TCP_CONN_STATE_RESET);
    struct send_buf_queue *sbq;

    u64 now_ms = tcp_net_ops->current_ms().ms;

    /* TCP keep-alive: probe idle ESTABLISHED connections (RFC 1122 §4.2.3.6).
     */
    if (conn->state == TCP_CONN_STATE_ESTABLISHED) {
        u64 idle_threshold = conn->keepalive_probes > 0
                                 ? TCP_CONN_KEEPALIVE_INTVL_MS
                                 : TCP_CONN_KEEPALIVE_IDLE_MS;
        if (now_ms - conn->last_activity_ms >= idle_threshold) {
            if (conn->keepalive_probes >= TCP_CONN_KEEPALIVE_CNT) {
                tcp_set_state(conn, TCP_CONN_STATE_RESET);
                TCP_NOTIFY(conn, on_disconnect);
                pr_debug(STR("Keep-alive timeout (%s). Resetting "
                             "connection.\n"),
                         tcp_conn_format(conn, &tmp));
                return result_ok();
            }
            struct send_buf probe_sb = send_buf_new(tmp);
            struct result ka_res = tcp_send_segment_noqueue(
                conn, TCP_HDR_FLAG_ACK, conn->send_next - 1,
                net_u16_from_u16(0), 0, probe_sb, tmp);
            if (ka_res.is_error)
                return ka_res;
            conn->keepalive_probes++;
            conn->last_activity_ms = now_ms;
            pr_debug(STR("Keep-alive probe #%u sent (%s)\n"),
                     conn->keepalive_probes, tcp_conn_format(conn, &tmp));
        }
    }

    /* Zero-window persist timer: when send window has been zero for one RTO,
     * retransmit the first queued segment as a probe. A pure ACK would be
     * silently dropped by the peer (RFC 793 forbids ACKing a pure ACK), so the
     * probe MUST carry at least one data byte to force the peer to ACK and
     * supply its current window.
     */
    if (conn->state == TCP_CONN_STATE_ESTABLISHED &&
        conn->zero_window_since_ms != 0 && conn->send_window_real == 0 &&
        now_ms - conn->zero_window_since_ms >= (u64) TCP_CONN_PERSIST_RTO_MS) {
        if (!list_empty(&conn->send_queue)) {
            struct send_buf_queue *first = __container_of(
                conn->send_queue.next, struct send_buf_queue, link);
            struct result persist_res = tcp_send_segment_noqueue(
                conn, first->flags, first->seq_num, first->checksum, first->len,
                first->sb, tmp);
            if (persist_res.is_error)
                return persist_res;
            pr_debug(STR("Zero-window persist probe sent (%s)\n"),
                     tcp_conn_format(conn, &tmp));
        }
        /* Reset persist timer to avoid empty-SBQ probe spam. */
        conn->zero_window_since_ms = now_ms;
    }

    list_for_each_entry_safe (&conn->send_queue, sbq, struct send_buf_queue,
                              link) {
        if (sbq->n_transmissions > 8) {
            if (tcp_conn_needs_user_close(conn)) {
                tcp_set_state(conn, TCP_CONN_STATE_RESET);
                TCP_NOTIFY(conn, on_disconnect);
                pr_debug(STR("Exceeded maximum number of retransmissions "
                             "(%s). Sending a reset.\n"),
                         tcp_conn_format(conn, &tmp));
                tcp_send_segment_noqueue(conn, TCP_HDR_FLAG_RST, sbq->seq_num,
                                         sbq->checksum, sbq->len, sbq->sb, tmp);
            } else {
                /* The connection is in a state where it is not accessible by
                 * the user. Delete the connection.
                 */
                pr_debug(STR("Exceeded maximum number of retransmissions "
                             "(%s). Deleted connection.\n"),
                         tcp_conn_format(conn, &tmp));
                tcp_free_conn(conn);
            }

            return result_ok();
        }

        /* Remove the buffer from the retransmission queue if the cumulative ACK
         * number received is greater than the last ACK number required by the
         * buffer. Alternatively, give up after a few retries.
         */
        if (seq_geq(conn->send_unack, sbq->required_ack)) {
            printk(KERN_VERBOSE, STR("Freeing sbq 0x%lx seq_num=%ld\n"), &sbq,
                   seq_num_relative(conn, sbq->seq_num));
            list_del(&sbq->link);
            tcp_free_sbq_and_sb(sbq);
            continue;
        }

        struct time_ms now = tcp_net_ops->current_ms();
        if (now.ms >= sbq->last_try.ms + sbq->retry_after.ms) {
            printk(KERN_VERBOSE,
                   STR("(%s) Retransmitting datagram seq_num=%ld "
                       "required_ack=%u n_transmissions=%ld now=%lu "
                       "last_try=%lu retry_after=%lu\n"),
                   tcp_conn_format(conn, &tmp),
                   seq_num_relative(conn, sbq->seq_num),
                   seq_num_relative(conn, sbq->required_ack),
                   sbq->n_transmissions, now.ms, sbq->last_try.ms,
                   sbq->retry_after.ms);
            struct result res =
                tcp_send_segment_noqueue(conn, sbq->flags, sbq->seq_num,
                                         sbq->checksum, sbq->len, sbq->sb, tmp);
            if (res.is_error)
                return res;
            sbq->n_transmissions++;
            sbq->retry_after = time_ms_new(sbq->retry_after.ms * 2);
            sbq->last_try = now;
            tcp_timer_hint(now.ms + sbq->retry_after.ms);
            __atomic_fetch_add(&global_tcp_stats_retransmits, 1,
                               __ATOMIC_RELAXED);
            conn->stats_retransmits++;
            conn->stats_pkts_sent++;
            conn->stats_bytes_sent += sbq->len;

            /* Notify congestion control of RTO once per loss epoch.
             * Guard: cwnd == mss means the window already collapsed for this
             * epoch; no need to recalculate ssthresh from the reduced cwnd.
             */
            if (sbq->n_transmissions == 2 && conn->cc && conn->cc->on_timeout &&
                conn->cwnd > tcp_cc_mss(conn))
                conn->cc->on_timeout(conn);
        }

        return result_ok();
    }

    return result_ok();
}

/* TCP timer min-heap.
 *
 * One heap slot per active connection. Each slot stores the minimum next-event
 * time across ALL of that connection's timers (retransmit, keep-alive, internal
 * timeout, persist). The heap is rebuilt via Floyd's O(N) algorithm after every
 * tcp_poll_retransmit() call so the global minimum is always O(1) accessible.
 */

struct tcp_timer_entry {
    u64 fire_ms;
    struct tcp_conn *conn;
};

/* One slot per connection; capacity matches the connection pool exactly. */
static struct tcp_timer_entry global_tcp_timer_heap[TCP_CONN_MAX_NUM];
static sz global_tcp_timer_heap_size;

static void tcp_heap_sift_down(sz i)
{
    for (;;) {
        sz left = 2 * i + 1;
        sz right = 2 * i + 2;
        sz min = i;

        if (left < global_tcp_timer_heap_size &&
            global_tcp_timer_heap[left].fire_ms <
                global_tcp_timer_heap[min].fire_ms)
            min = left;
        if (right < global_tcp_timer_heap_size &&
            global_tcp_timer_heap[right].fire_ms <
                global_tcp_timer_heap[min].fire_ms)
            min = right;
        if (min == i)
            break;

        struct tcp_timer_entry tmp_e = global_tcp_timer_heap[i];
        global_tcp_timer_heap[i] = global_tcp_timer_heap[min];
        global_tcp_timer_heap[min] = tmp_e;
        i = min;
    }
}

/* Compute the earliest millisecond timestamp at which conn needs service.
 * Returns TIME_MS_MAX when no timer is armed.
 */
static u64 tcp_conn_next_event_ms(struct tcp_conn *conn)
{
    u64 next = TIME_MS_MAX;
    struct send_buf_queue *sbq;

    /* Internal timeout (TIME_WAIT, SYN_RCVD, LAST_ACK expiry, etc.). */
    if (conn->internal_timeout.ms < next)
        next = conn->internal_timeout.ms;

    if (conn->state == TCP_CONN_STATE_ESTABLISHED) {
        /* Keep-alive probe deadline. Guard against u64 wrap. */
        u64 idle = conn->keepalive_probes > 0
                       ? (u64) TCP_CONN_KEEPALIVE_INTVL_MS
                       : (u64) TCP_CONN_KEEPALIVE_IDLE_MS;
        u64 ka_fire = conn->last_activity_ms + idle;
        if (ka_fire < conn->last_activity_ms)
            ka_fire = TIME_MS_MAX; /* overflow */
        if (ka_fire < next)
            next = ka_fire;

        /* Zero-window persist timer. */
        if (conn->zero_window_since_ms != 0) {
            u64 persist_fire =
                conn->zero_window_since_ms + (u64) TCP_CONN_PERSIST_RTO_MS;
            if (persist_fire < conn->zero_window_since_ms)
                persist_fire = TIME_MS_MAX;
            if (persist_fire < next)
                next = persist_fire;
        }
    }

    /* Retransmit timers: earliest unacknowledged segment due for retry. */
    list_for_each_entry_safe (&conn->send_queue, sbq, struct send_buf_queue,
                              link) {
        if (!seq_geq(conn->send_unack, sbq->required_ack)) {
            u64 retx_fire = sbq->last_try.ms + sbq->retry_after.ms;
            if (retx_fire < sbq->last_try.ms)
                retx_fire = TIME_MS_MAX; /* overflow */
            if (retx_fire < next)
                next = retx_fire;
        }
    }

    return next;
}

/* Rebuild the min-heap from scratch using Floyd's O(N) heapification. */
static void tcp_heap_rebuild(void)
{
    global_tcp_timer_heap_size = 0;

    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *conn = &global_tcp_conn_table[i];
        if (!conn->is_used || conn->state == TCP_CONN_STATE_RESET)
            continue;

        u64 fire = tcp_conn_next_event_ms(conn);
        if (fire == TIME_MS_MAX)
            continue; /* No armed timer for this connection. */

        global_tcp_timer_heap[global_tcp_timer_heap_size].fire_ms = fire;
        global_tcp_timer_heap[global_tcp_timer_heap_size].conn = conn;
        global_tcp_timer_heap_size++;
    }

    /* Floyd's bottom-up O(N) heapification. */
    for (sz i = global_tcp_timer_heap_size / 2; i-- > 0;)
        tcp_heap_sift_down(i);

    __atomic_store_n(&global_tcp_next_timer_ms,
                     (global_tcp_timer_heap_size == 0)
                         ? TIME_MS_MAX
                         : global_tcp_timer_heap[0].fire_ms,
                     __ATOMIC_RELEASE);
}

/* Return the millisecond timestamp of the next TCP timer event across all
 * connections.
 * Returns TIME_MS_MAX when no timers are pending.
 */
u64 tcp_timer_next_ms(void)
{
    return __atomic_load_n(&global_tcp_next_timer_ms, __ATOMIC_ACQUIRE);
}

static struct result tcp_poll_retransmit_locked(struct arena tmp)
{
    /* Service only connections whose earliest timer has actually expired.
     * The heap root identifies the next due connection in O(1); rebuilding
     * after each serviced connection keeps the root current even if the
     * handler freed the connection or re-armed a different timer.
     */
    /* Rebuild the heap from the live connection table on every poll.
     * This ensures heap entries always reference the correct connection —
     * a freed-and-reused pool slot cannot produce a stale match.
     * O(N) on the connection table, bounded by TCP_CONN_MAX_NUM.
     */
    tcp_heap_rebuild();

    while (global_tcp_timer_heap_size > 0) {
        struct tcp_timer_entry root = global_tcp_timer_heap[0];
        u64 now_ms = tcp_net_ops->current_ms().ms;
        if (root.fire_ms > now_ms)
            break;

        struct tcp_conn *conn = root.conn;
        if (!conn || !conn->is_used || conn->state == TCP_CONN_STATE_RESET) {
            tcp_heap_rebuild();
            continue;
        }

        /* Internal timeout (TIME_WAIT, SYN_RCVD, LAST_ACK, FIN_WAIT, etc.):
         * free or reset the connection so it doesn't block the heap root.
         */
        if (tcp_conn_is_internal(conn) && now_ms >= conn->internal_timeout.ms) {
            tcp_free_conn(conn);
            tcp_heap_rebuild();
            continue;
        }
        if (conn->state == TCP_CONN_STATE_SYN_SENT &&
            now_ms >= conn->internal_timeout.ms) {
            tcp_set_state(conn, TCP_CONN_STATE_RESET);
            tcp_heap_rebuild();
            continue;
        }

        struct result res = tcp_poll_retransmit_conn(conn, tmp);
        tcp_heap_rebuild();
        if (res.is_error)
            return res;
    }

    return result_ok();
}

struct result tcp_poll_retransmit(struct arena tmp)
{
    assert(global_tcp_is_initialized);

    /* Fast path: if a known future deadline exists and hasn't arrived yet,
     * skip the pool lock and full connection scan.  TIME_MS_MAX means no
     * cached deadline — fall through so tcp_heap_rebuild discovers newly
     * armed timers that haven't been reflected in the cache yet.
     */
    u64 cached = tcp_timer_next_ms();
    u64 now_ms = tcp_net_ops->current_ms().ms;
    if (cached != TIME_MS_MAX && cached > now_ms)
        return result_ok();

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    struct result ret = tcp_poll_retransmit_locked(tmp);
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

static struct result tcp_send_segment(struct tcp_conn *conn,
                                      u8 flags,
                                      struct byte_view fragment,
                                      struct arena tmp)
{
    assert(conn);

    u32 seq_num = conn->send_next;
    u32 advance = fragment.len;
    if (flags & TCP_HDR_FLAG_SYN)
        advance++;
    if (flags & TCP_HDR_FLAG_FIN)
        advance++;

    struct send_buf_queue *sbq = tcp_alloc_sbq_and_sb();
    if (!sbq) {
        pr_debug(STR("TCP: Failed to allocate send buffer queue for %s\n"),
                 tcp_conn_format(conn, &tmp));
        return result_error(ENOMEM);
    }

    conn->send_next += advance;
    sbq->seq_num = seq_num;
    sbq->required_ack = conn->send_next;
    sbq->flags = flags;
    sbq->last_try = tcp_net_ops->current_ms();
    sbq->retry_after = time_ms_new(conn->rto);
    sbq->n_transmissions = 1;
    tcp_timer_hint(sbq->last_try.ms + sbq->retry_after.ms);
    sbq->checksum = internet_checksum_iterate(net_u16_from_u16(0), fragment);
    sbq->len = fragment.len;

    if (fragment.len) {
        struct byte_buf *buf = send_buf_prepend(&sbq->sb, fragment.len);
        assert(byte_buf_append(buf, fragment) == fragment.len);
    }

    list_add_tail(&conn->send_queue, &sbq->link);

    conn->stats_pkts_sent++;
    conn->stats_bytes_sent += fragment.len;
    return tcp_send_segment_noqueue(conn, sbq->flags, sbq->seq_num,
                                    sbq->checksum, sbq->len, sbq->sb, tmp);
}

static inline struct result tcp_send_segment_empty(struct tcp_conn *conn,
                                                   u8 flags,
                                                   struct arena arn)
{
    return tcp_send_segment(conn, flags, byte_view_new(NULL, 0), arn);
}

/* Handling incoming segments and manage the TCP state machine */

static struct result tcp_handle_receive_listen(struct tcp_conn *listen_conn,
                                               struct ipv4_addr peer_addr,
                                               u16 peer_port,
                                               struct tcp_header *hdr,
                                               struct tsopt tsopt,
                                               struct send_buf sb,
                                               struct arena tmp)
{
    assert(listen_conn);
    assert(hdr);
    assert(listen_conn->state == TCP_CONN_STATE_LISTEN);

    /* RST is ignored for connections in the listen state because these
     * connections aren't really connected to anything yet (they're just waiting
     * to connect).
     */
    if (hdr->flags & TCP_HDR_FLAG_RST)
        return result_ok();

    if (!(hdr->flags & TCP_HDR_FLAG_SYN))
        return result_ok(); /* Only SYNs are relevant in the LISTEN state. */

    /* When in LISTEN state, the connection doesn't known about the peer yet. So
     * the peer fields must be wildcards.
     */
    assert(ipv4_addr_is_equal(listen_conn->peer_addr, IPV4_ADDR_ANY));
    assert(listen_conn->peer_port == 0);

    /* A new connection is created now so that 'listen_conn' can remain in the
     * LISTEN state to accept new connections. The new connection will be moved
     * through the states of the TCP handshake until it's in the ESTABLISHED
     * state.
     */

    /* Per-source-IP limits: count half-open and active (non-closing)
     * connections from this peer. Connections in teardown states (FIN_WAIT_1/2,
     * CLOSING, TIME_WAIT, LAST_ACK) are excluded because they no longer consume
     * server resources; counting them would cause the limit to be reached after
     * just a few HTTP keep-alive cycles (8 connections in ~2 seconds, then
     * blocked for the 30-second internal timeout).
     */
    sz syn_rcvd_from_peer = 0;
    sz total_conns_from_peer = 0;
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *c = &global_tcp_conn_table[i];
        if (!c->is_used || !ipv4_addr_is_equal(c->peer_addr, peer_addr))
            continue;
        if (c->state == TCP_CONN_STATE_SYN_RCVD)
            syn_rcvd_from_peer++;
        if (!tcp_conn_is_internal(c))
            total_conns_from_peer++;
    }

    /* Half-open (SYN flood) guard: silent drop. */
    if (syn_rcvd_from_peer >= TCP_MAX_SYN_RCVD_PER_IP) {
        pr_warn(STR("TCP: SYN flood guard: dropping SYN from %s:%hu (>=%d "
                    "half-open)\n"),
                ipv4_addr_format(peer_addr, &tmp), peer_port,
                (u64) TCP_MAX_SYN_RCVD_PER_IP);
        return result_ok();
    }

    /* Per-source-IP connection cap: reject with RST so the client gets
     * immediate feedback.
     */
    if (total_conns_from_peer >= TCP_MAX_CONNS_PER_IP) {
        pr_warn(STR("TCP: Per-IP connection limit: sending RST to %s:%hu (%d "
                    "conns >= limit %d)\n"),
                ipv4_addr_format(peer_addr, &tmp), peer_port,
                (u64) total_conns_from_peer, (u64) TCP_MAX_CONNS_PER_IP);
        /* RFC 793 §3.4: if incoming has ACK, RST SEQ=ACK_field, no ACK in
         * response. If incoming has no ACK (SYN only, payload not available
         * here), RST|ACK with SEQ=0, ACK=SEQ+1 (SYN consumes 1 sequence
         * number).
         */
        u32 rst_seq = (hdr->flags & TCP_HDR_FLAG_ACK)
                          ? u32_from_net_u32(hdr->ack_num)
                          : 0;
        u32 rst_ack = (hdr->flags & TCP_HDR_FLAG_ACK)
                          ? 0
                          : u32_from_net_u32(hdr->seq_num) + 1u;
        u8 rst_flags =
            TCP_HDR_FLAG_RST |
            ((hdr->flags & TCP_HDR_FLAG_ACK) ? 0u : TCP_HDR_FLAG_ACK);
        return tcp_send_segment_noqueue_raw(
            listen_conn->host_addr, peer_addr, listen_conn->host_port,
            peer_port, rst_seq, rst_ack, 0, 0, false, rst_flags,
            net_u16_from_u16(0), 0, 0, false, sb, tmp);
    }

    struct tcp_conn *conn =
        tcp_conn_alloc_and_init(listen_conn->host_addr, listen_conn->host_port,
                                listen_conn->mss, TCP_CONN_STATE_SYN_RCVD);
    if (!conn) {
        __atomic_fetch_add(&global_tcp_stats_pool_exhaustion, 1,
                           __ATOMIC_RELAXED);
        pr_warn(STR("TCP: Connection pool exhausted: dropping SYN from "
                    "%s:%hu\n"),
                ipv4_addr_format(peer_addr, &tmp), peer_port);
        return result_ok();
    }

    list_add(&listen_conn->accept_queue, &conn->accept_queue);
    conn->listener = listen_conn;

    conn->peer_addr = peer_addr;
    conn->peer_port = peer_port;
    tcp_hash_insert(conn);
    /* The SYN in the incoming header has consumed one sequence number, so add
     * one to the ISN sent by the peer.
     */
    conn->recv_next = u32_from_net_u32(hdr->seq_num) + 1;
    conn->send_window_scale = listen_conn->send_window_scale;
    conn->use_window_scale = listen_conn->use_window_scale;

    tcp_conn_set_internal_timeout(conn);

    if (tsopt.have_ts) {
        conn->use_time_stamps = true;
        conn->ts_recent = tsopt.tsval;
    }

    pr_debug(STR("Received SYN for a connection in the LISTEN state (%s). "
                 "Responding with SYN + ACK. Created a new connection in the "
                 "SYN_RCVD state.\n"),
             tcp_conn_format(conn, &tmp));

    return tcp_send_segment_empty(conn, TCP_HDR_FLAG_SYN | TCP_HDR_FLAG_ACK,
                                  tmp);
}

/* Active-open state handler: process SYN-ACK from the peer. */
static struct result tcp_handle_receive_syn_sent(struct tcp_conn *conn,
                                                 struct tcp_header *hdr,
                                                 struct tsopt tsopt,
                                                 struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_SYN_SENT);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        /* Connection refused. */
        tcp_set_state(conn, TCP_CONN_STATE_RESET);
        pr_debug(STR("Connection refused (RST) for %s\n"),
                 tcp_conn_format(conn, &tmp));
        return result_ok();
    }

    /* Only SYN-ACK is relevant here. */
    if (!((hdr->flags & TCP_HDR_FLAG_SYN) && (hdr->flags & TCP_HDR_FLAG_ACK)))
        return result_ok();

    /* Verify the ACK acknowledges the SYN: must be iss+1.
     * RFC 9293 §3.10.7.2: unacceptable ACK in SYN_SENT → send RST.
     */
    u32 ack_num = u32_from_net_u32(hdr->ack_num);
    if (ack_num != conn->iss + 1) {
        pr_debug(STR("SYN-ACK with bad ack %u (expected %u) for %s\n"), ack_num,
                 conn->iss + 1, tcp_conn_format(conn, &tmp));
        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_RST, tmp);
    }

    /* Apply peer's options (MSS, WS already parsed by tcp_handle_options). */
    tcp_conn_update_send_state(conn, hdr, tsopt);

    /* SYN consumes one sequence number. */
    conn->recv_next = u32_from_net_u32(hdr->seq_num) + 1;

    /* Timestamps negotiation. */
    if (tsopt.have_ts) {
        conn->use_time_stamps = true;
        conn->ts_recent = tsopt.tsval;
    }

    /* Allocate receive buffer for the ESTABLISHED state. */
    struct result buf_res =
        recv_buf_alloc(&conn->recv_buf, &global_tcp_recv_buf_alloc);
    if (buf_res.is_error) {
        pr_warn(STR("Failed to allocate recv_buf for %s. Resetting.\n"),
                tcp_conn_format(conn, &tmp));
        tcp_send_segment_empty(conn, TCP_HDR_FLAG_RST, tmp);
        tcp_set_state(conn, TCP_CONN_STATE_RESET);
        return result_error(ENOMEM);
    }

    tcp_set_state(conn, TCP_CONN_STATE_ESTABLISHED);
    if (conn->listener) {
        u64 listener_flags = spin_lock_irqsave(&conn->listener->lock);
        conn->listener->accept_ready++;
        spin_unlock_irqrestore(&conn->listener->lock, listener_flags);
    }
    TCP_NOTIFY(conn, on_establish);
    /* Clear the SYN_SENT timeout. */
    conn->internal_timeout = time_ms_new(TIME_MS_MAX);

    pr_debug(STR("SYN-ACK received, connection ESTABLISHED for %s\n"),
             tcp_conn_format(conn, &tmp));

    /* Complete the three-way handshake: send ACK. */
    return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
}

static struct result tcp_handle_receive_syn_rcvd(struct tcp_conn *conn,
                                                 struct tcp_header *hdr,
                                                 struct tsopt tsopt,
                                                 struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_SYN_RCVD);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        /* Connections in the SYN_RECV state are in the processes of
         * establishing the connection. They can't be used yet and users of the
         * TCP API can't access them.  Delete the connection.
         */
        tcp_free_conn(conn);
        return result_ok();
    }

    /* A SYN is always sent before moving to the SYN_RCVD state.  At this
     * point only an ACK for that SYN matters.  Validate that the ACK
     * acknowledges our SYN (ISS + 1) per RFC 793 Section 3.4.
     */
    if (!(hdr->flags & TCP_HDR_FLAG_ACK))
        return result_ok();

    u32 ack_num = u32_from_net_u32(hdr->ack_num);
    if (ack_num != conn->iss + 1)
        return result_ok();

    tcp_set_state(conn, TCP_CONN_STATE_ESTABLISHED);
    if (conn->listener) {
        u64 listener_flags = spin_lock_irqsave(&conn->listener->lock);
        conn->listener->accept_ready++;
        spin_unlock_irqrestore(&conn->listener->lock, listener_flags);
    }
    TCP_NOTIFY(conn, on_establish);
    tcp_conn_update_send_state(conn, hdr, tsopt);

    /* Data reception begins in the ESTABLISHED state, so allocate a
     * receive buffer at this point.
     */
    struct result buf_alloc_res =
        recv_buf_alloc(&conn->recv_buf, &global_tcp_recv_buf_alloc);
    if (buf_alloc_res.is_error) {
        pr_warn(STR("Failed to allocate receive buffer for a connection "
                    "(%s). Resetting and deleting the connection.\n"),
                tcp_conn_format(conn, &tmp));
        tcp_send_segment_empty(conn, TCP_HDR_FLAG_RST, tmp);
        tcp_free_conn(conn);
        return result_error(ENOMEM);
    }

    pr_debug(STR("Received ACK for a connection in the SYN_RCVD state (%s). "
                 "Not responding. The connection is ESTABLISHED now.\n"),
             tcp_conn_format(conn, &tmp));

    return result_ok();
}

static struct result tcp_handle_receive_established(struct tcp_conn *conn,
                                                    struct tcp_header *hdr,
                                                    struct tsopt tsopt,
                                                    struct byte_view payload,
                                                    struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_ESTABLISHED);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        tcp_set_state(conn, TCP_CONN_STATE_RESET);
        TCP_NOTIFY(conn, on_disconnect);

        pr_debug(STR("Received RST for a connection in the ESTABLISHED state "
                     "(%s). Not responding. The connection is in the RESET "
                     "state now.\n"),
                 tcp_conn_format(conn, &tmp));

        return result_ok();
    }

    tcp_conn_update_send_state(conn, hdr, tsopt);
    sz n_received = tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

    if (hdr->flags & TCP_HDR_FLAG_FIN) {
        tcp_set_state(conn, TCP_CONN_STATE_CLOSE_WAIT);
        TCP_NOTIFY(conn, on_disconnect);

        pr_debug(STR("Received FIN for a connection in the ESTABLISHED state "
                     "(%s). Responding with ACK. The connection is in the "
                     "CLOSE_WAIT state now.\n"),
                 tcp_conn_format(conn, &tmp));

        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
    }

    if (n_received > 0 || payload.len > 0) {
        /* ACK any data-carrying segment.  When n_received == 0 the data was
         * dropped or out-of-order; this DUPACK triggers Fast Retransmit at the
         * sender (RFC 5681 §3.2).
         */
        pr_debug(STR("Received %ld bytes of data for connection %s. "
                     "Responding with ACK.\n"),
                 n_received, tcp_conn_format(conn, &tmp));
        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
    }

    return result_ok();
}

static void tcp_handle_receive_last_ack(struct tcp_conn *conn,
                                        struct tcp_header *hdr,
                                        struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_LAST_ACK);

    if ((hdr->flags & TCP_HDR_FLAG_ACK) || (hdr->flags & TCP_HDR_FLAG_RST)) {
        pr_debug(STR("Received an ACK or RST (flags=%hhu) for a connection in "
                     "the LAST_ACK state (%s). Not responding. The connection "
                     "is deleted now.\n"),
                 hdr->flags, tcp_conn_format(conn, &tmp));

        tcp_free_conn(conn);
    }
}

static struct result tcp_handle_receive_fin_wait_1(struct tcp_conn *conn,
                                                   struct tcp_header *hdr,
                                                   struct tsopt tsopt,
                                                   struct byte_view payload,
                                                   struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_FIN_WAIT_1);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        pr_debug(STR("Received RST for a connection in the FIN_WAIT_1 state "
                     "(%s). Not responding. The connection is deleted now.\n"),
                 tcp_conn_format(conn, &tmp));
        tcp_free_conn(conn);
        return result_ok();
    }

    if ((hdr->flags & TCP_HDR_FLAG_FIN) && (hdr->flags & TCP_HDR_FLAG_ACK)) {
        tcp_set_state(conn, TCP_CONN_STATE_TIME_WAIT);

        tcp_conn_set_internal_timeout(conn);
        tcp_conn_update_send_state(conn, hdr, tsopt);
        tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

        pr_debug(STR("Received FIN + ACK for a connection in the FIN_WAIT_1 "
                     "state (%s). Responding with ACK. The connection is in "
                     "the TIME_WAIT state now.\n"),
                 tcp_conn_format(conn, &tmp));

        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
    } else if (hdr->flags & TCP_HDR_FLAG_FIN) {
        tcp_set_state(conn, TCP_CONN_STATE_CLOSING);

        tcp_conn_set_internal_timeout(conn);
        tcp_conn_update_send_state(conn, hdr, tsopt);
        tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

        pr_debug(STR("Received FIN for a connection in the FIN_WAIT_1 state "
                     "(%s). Responding with ACK. The connection is in the "
                     "CLOSING state now.\n"),
                 tcp_conn_format(conn, &tmp));

        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
    } else if (hdr->flags & TCP_HDR_FLAG_ACK) {
        tcp_set_state(conn, TCP_CONN_STATE_FIN_WAIT_2);

        tcp_conn_set_internal_timeout(conn);
        tcp_conn_update_send_state(conn, hdr, tsopt);
        tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

        pr_debug(STR("Received ACK for a connection in the FIN_WAIT_1 state "
                     "(%s). Not responding. The connection is in the "
                     "FIN_WAIT_2 state now.\n"),
                 tcp_conn_format(conn, &tmp));

        return result_ok();
    }

    /* Stay in the FIN_WAIT_1 state if neither an ACK, nor a FIN, nor both were
     * received.
     */
    return result_ok();
}

static struct result tcp_handle_receive_fin_wait_2(struct tcp_conn *conn,
                                                   struct tcp_header *hdr,
                                                   struct tsopt tsopt,
                                                   struct byte_view payload,
                                                   struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_FIN_WAIT_2);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        pr_debug(STR("Received RST for a connection in the FIN_WAIT_2 state "
                     "(%s). Not responding. The connection is deleted now.\n"),
                 tcp_conn_format(conn, &tmp));
        tcp_free_conn(conn);
        return result_ok();
    }

    /* The connection is half open in the FIN_WAIT_2 state. The state of the
     * connection must be updated so remaining data is retransmitted correctly.
     */
    tcp_conn_update_send_state(conn, hdr, tsopt);
    tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

    if (!(hdr->flags & TCP_HDR_FLAG_FIN))
        return result_ok();

    tcp_set_state(conn, TCP_CONN_STATE_TIME_WAIT);
    tcp_conn_set_internal_timeout(conn);

    pr_debug(STR("Received FIN for a connection in the FIN_WAIT_2 state (%s). "
                 "Responding with ACK. The connection is in the TIME_WAIT "
                 "state now.\n"),
             tcp_conn_format(conn, &tmp));

    return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
}

static void tcp_handle_receive_closing(struct tcp_conn *conn,
                                       struct tcp_header *hdr,
                                       struct tsopt tsopt,
                                       struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_CLOSING);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        pr_debug(STR("Received RST for a connection in the CLOSING state "
                     "(%s). Not responding. The connection is deleted now.\n"),
                 tcp_conn_format(conn, &tmp));
        tcp_free_conn(conn);
        return;
    }

    if (!(hdr->flags & TCP_HDR_FLAG_ACK))
        return;

    tcp_set_state(conn, TCP_CONN_STATE_TIME_WAIT);

    tcp_conn_set_internal_timeout(conn);
    tcp_conn_update_send_state(conn, hdr, tsopt);

    pr_debug(STR("Received ACK for a connection in the CLOSING state (%s). "
                 "Not responding. The connection is in the TIME_WAIT state "
                 "now.\n"),
             tcp_conn_format(conn, &tmp));
}

static struct result tcp_handle_receive_time_wait(struct tcp_conn *conn,
                                                  struct tcp_header *hdr,
                                                  struct tsopt tsopt,
                                                  struct byte_view payload,
                                                  struct arena tmp)
{
    assert(conn);
    assert(hdr);
    assert(conn->state == TCP_CONN_STATE_TIME_WAIT);

    if (hdr->flags & TCP_HDR_FLAG_RST) {
        pr_debug(STR("Received RST for a connection in the TIME_WAIT state "
                     "(%s). Not responding. The connection is deleted now.\n"),
                 tcp_conn_format(conn, &tmp));
        tcp_free_conn(conn);
        return result_ok();
    }

    /* No user will ever see data received here.  The only purpose of
     * updating the send and receive states at this point is to ensure
     * that all data from the peer is ACKed when returning from this
     * function.
     */
    tcp_conn_update_send_state(conn, hdr, tsopt);
    tcp_conn_update_recv_state(conn, hdr, tsopt, payload, tmp);

    if (!(hdr->flags & TCP_HDR_FLAG_FIN))
        return result_ok();

    pr_debug(STR("Received FIN for a connection in the TIME_WAIT state (%s). "
                 "Responding with ACK. The connection remains in the "
                 "TIME_WAIT state.\n"),
             tcp_conn_format(conn, &tmp));

    return tcp_send_segment_empty(conn, TCP_HDR_FLAG_ACK, tmp);
}

static bool tcp_checksum_is_ok(struct tcp_ip_pseudo_header pseudo_hdr,
                               struct byte_view segment)
{
    net_u16 checksum = net_u16_from_u16(0);
    checksum = internet_checksum_iterate(
        checksum, byte_view_new((void *) &pseudo_hdr, sizeof(pseudo_hdr)));
    checksum = internet_checksum_iterate(checksum, segment);
    return internet_checksum_finalize(checksum).inner == 0;
}

static struct tsopt tcp_handle_options(struct tcp_conn *conn,
                                       u8 flags,
                                       struct byte_view opts)
{
    struct tsopt tsopt = {.have_ts = false, .tsval = 0, .tsecr = 0};

    sz i = 0;
    while (i < opts.len) {
        switch (opts.dat[i]) {
        case TCP_OPT_EOL_KIND:
            return tsopt;
        case TCP_OPT_NOP_KIND:
            i++;
            break;
        case TCP_OPT_MSS_KIND:
            /* The length field can be ignored because it is always 4. */
            if (i + TCP_OPT_MSS_LENGTH > opts.len)
                return tsopt;
            conn->mss = ((u16) opts.dat[i + 2] << 8) | (u16) opts.dat[i + 3];
            if (conn->mss == 0)
                conn->mss = TCP_CONN_DEFAULT_MSS;
            i += TCP_OPT_MSS_LENGTH;
            break;
        case TCP_OPT_WS_KIND:
            if (i + TCP_OPT_WS_LENGTH > opts.len)
                return tsopt;
            if ((flags & TCP_HDR_FLAG_SYN) && opts.dat[i + 2] <= 14) {
                conn->send_window_scale = opts.dat[i + 2];
                conn->use_window_scale = true;
            }
            i += TCP_OPT_WS_LENGTH;
            break;
        case TCP_OPT_TS_KIND:
            if (i + TCP_OPT_TS_LENGTH > opts.len)
                return tsopt;
            tsopt.have_ts = true;
            tsopt.tsval = ((u32) opts.dat[i + 2] << 24) |
                          ((u32) opts.dat[i + 3] << 16) |
                          ((u32) opts.dat[i + 4] << 8) | (u32) opts.dat[i + 5];
            tsopt.tsecr = ((u32) opts.dat[i + 6] << 24) |
                          ((u32) opts.dat[i + 7] << 16) |
                          ((u32) opts.dat[i + 8] << 8) | (u32) opts.dat[i + 9];
            i += TCP_OPT_TS_LENGTH;
            break;
#if CONFIG_TCP_SACK
        case TCP_OPT_SACK_PERMITTED_KIND:
            if (i + TCP_OPT_SACK_PERMITTED_LENGTH > opts.len)
                return tsopt;
            if (flags & TCP_HDR_FLAG_SYN)
                tsopt.have_sack_permitted = true;
            i += TCP_OPT_SACK_PERMITTED_LENGTH;
            break;
        case TCP_OPT_SACK_KIND: {
            if (i + 2 > opts.len)
                return tsopt;
            u8 sack_len = opts.dat[i + 1];
            if (sack_len < 2 || i + sack_len > opts.len)
                return tsopt;
            /* Each SACK block is 8 bytes (left_edge + right_edge). */
            u8 n_blocks = (u8) ((sack_len - 2) / 8);
            if (n_blocks > TCP_SACK_MAX_BLOCKS)
                n_blocks = TCP_SACK_MAX_BLOCKS;
            tsopt.n_sack_blocks = n_blocks;
            sz off = i + 2;
            for (u8 b = 0; b < n_blocks; b++) {
                tsopt.sack_blocks[b].left_edge =
                    ((u32) opts.dat[off] << 24) |
                    ((u32) opts.dat[off + 1] << 16) |
                    ((u32) opts.dat[off + 2] << 8) | (u32) opts.dat[off + 3];
                tsopt.sack_blocks[b].right_edge =
                    ((u32) opts.dat[off + 4] << 24) |
                    ((u32) opts.dat[off + 5] << 16) |
                    ((u32) opts.dat[off + 6] << 8) | (u32) opts.dat[off + 7];
                off += 8;
            }
            i += sack_len;
            break;
        }
#endif /* CONFIG_TCP_SACK */
        default:
            /* All options except for EOL and NOP have a "length" field after
             * the "kind" field.  Use the "length" field to skip the rest of
             * this option.
             */
            if (i + 2 > opts.len)
                return tsopt;
            /* RFC 9293: the minimum option length is 2 (kind + length bytes
             * themselves). A length of 0 or 1 would cause the index to stall or
             * regress, looping forever.
             */
            if (opts.dat[i + 1] < 2)
                return tsopt;
            i += opts.dat[i + 1];
            break;
        }
    }

    return tsopt;
}

static struct result tcp_handle_packet_locked(struct byte_view segment,
                                              struct tcp_header *tcp_hdr,
                                              struct byte_view payload,
                                              struct ipv4_addr host_addr,
                                              struct ipv4_addr peer_addr,
                                              u16 host_port,
                                              u16 peer_port,
                                              struct send_buf sb,
                                              struct arena tmp)
{
    struct tcp_conn *conn =
        tcp_lookup_conn(host_addr, peer_addr, host_port, peer_port);

    if (!conn) {
        /* No matching connection - send RST to reset the peer.
         * This is standard TCP behavior for unexpected segments.
         */
        pr_debug(STR("TCP: No connection found for segment from %s:%hu -> "
                     "%s:%hu. "
                     "Sending RST.\n"),
                 ipv4_addr_format(peer_addr, &tmp), peer_port,
                 ipv4_addr_format(host_addr, &tmp), host_port);
        /* RFC 793 §3.4: RST generation for segments to unknown connections.
         * If ACK is set: SEQ = SEG.ACK, flags = RST.
         * If ACK is not set: SEQ = 0, ACK = SEG.SEQ + SEG.LEN, flags = RST|ACK.
         */
        if (tcp_hdr->flags & TCP_HDR_FLAG_ACK) {
            return tcp_send_segment_noqueue_raw(
                host_addr, peer_addr, host_port, peer_port,
                u32_from_net_u32(tcp_hdr->ack_num), 0, 0, 0, false,
                TCP_HDR_FLAG_RST, net_u16_from_u16(0), 0, 0, false, sb, tmp);
        }
        u32 seg_len = payload.len;
        if (tcp_hdr->flags & TCP_HDR_FLAG_SYN)
            seg_len++;
        if (tcp_hdr->flags & TCP_HDR_FLAG_FIN)
            seg_len++;
        return tcp_send_segment_noqueue_raw(
            host_addr, peer_addr, host_port, peer_port, 0,
            u32_from_net_u32(tcp_hdr->seq_num) + seg_len, 0, 0, false,
            TCP_HDR_FLAG_RST | TCP_HDR_FLAG_ACK, net_u16_from_u16(0), 0, 0,
            false, sb, tmp);
    }

    conn->last_activity_ms = tcp_net_ops->current_ms().ms;
    conn->keepalive_probes = 0;

    struct tsopt tsopt = {.have_ts = false, .tsval = 0, .tsecr = 0};

    /* Security: Parse TCP options with strict bounds checking */
    if (tcp_hdr->header_len > TCP_HDR_LEN_NO_OPT) {
        sz options_start = (sz) TCP_HDR_LEN_NO_OPT * 4;
        sz options_len = (sz) (tcp_hdr->header_len - TCP_HDR_LEN_NO_OPT) * 4;

        /* Double-check bounds before parsing options */
        if (options_start + options_len > (sz) segment.len) {
            pr_debug(
                STR("TCP options extend beyond segment "
                    "boundary. "
                    "Dropping ...\n"));
            return result_ok();
        }

        struct byte_view tcp_options =
            byte_view_new(segment.dat + options_start, options_len);
        tsopt = tcp_handle_options(conn, tcp_hdr->flags, tcp_options);
    }

    conn->stats_pkts_recv++;
    conn->stats_bytes_recv += payload.len;

    switch (conn->state) {
    case TCP_CONN_STATE_LISTEN:
        return tcp_handle_receive_listen(conn, peer_addr, peer_port, tcp_hdr,
                                         tsopt, sb, tmp);
    case TCP_CONN_STATE_SYN_SENT:
        return tcp_handle_receive_syn_sent(conn, tcp_hdr, tsopt, tmp);
    case TCP_CONN_STATE_SYN_RCVD:
        return tcp_handle_receive_syn_rcvd(conn, tcp_hdr, tsopt, tmp);
    case TCP_CONN_STATE_ESTABLISHED:
        return tcp_handle_receive_established(conn, tcp_hdr, tsopt, payload,
                                              tmp);
    case TCP_CONN_STATE_CLOSE_WAIT:
        /* RFC 793: ACK any segment received in CLOSE_WAIT. */
        return tcp_send_segment_noqueue(conn, TCP_HDR_FLAG_ACK, conn->send_next,
                                        net_u16_from_u16(0), 0, sb, tmp);
    case TCP_CONN_STATE_LAST_ACK:
        tcp_handle_receive_last_ack(conn, tcp_hdr, tmp);
        return result_ok();
    case TCP_CONN_STATE_FIN_WAIT_1:
        return tcp_handle_receive_fin_wait_1(conn, tcp_hdr, tsopt, payload,
                                             tmp);
    case TCP_CONN_STATE_FIN_WAIT_2:
        return tcp_handle_receive_fin_wait_2(conn, tcp_hdr, tsopt, payload,
                                             tmp);
    case TCP_CONN_STATE_CLOSING:
        tcp_handle_receive_closing(conn, tcp_hdr, tsopt, tmp);
        return result_ok();
    case TCP_CONN_STATE_TIME_WAIT:
        return tcp_handle_receive_time_wait(conn, tcp_hdr, tsopt, payload, tmp);
    case TCP_CONN_STATE_RESET:
        return result_ok();
    default:
        pr_err(STR("Unknown connection state %d for %s.\n"), conn->state,
               tcp_conn_format_raw(host_addr, peer_addr, host_port, peer_port,
                                   &tmp));
        crash("Connection state invalid");
    }
}

struct result tcp_handle_packet(struct tcp_ip_pseudo_header pseudo_hdr,
                                struct byte_view segment,
                                struct send_buf sb,
                                struct arena tmp)
{
    assert(global_tcp_is_initialized);
    sched_note_activity();

    if (segment.dat == NULL) {
        pr_debug(STR("Received NULL TCP segment. Dropping ...\n"));
        return result_ok();
    }
    if (segment.len < 0) {
        pr_debug(
            STR("Received TCP segment with negative length. Dropping ...\n"));
        return result_ok();
    }
    if (segment.len < sizeof(struct tcp_header)) {
        pr_debug(STR("Received TCP segment smaller than the TCP header "
                     "(%ld < %lu). Dropping ...\n"),
                 segment.len, sizeof(struct tcp_header));
        return result_ok();
    }

    struct tcp_header *tcp_hdr = byte_view_ptr(segment);
    if (tcp_hdr->header_len < TCP_HDR_LEN_NO_OPT) {
        pr_debug(STR("Received TCP segment with invalid header length %hhd "
                     "(must be at least " TOSTRING(
                         TCP_HDR_LEN_NO_OPT) "). "
                                             "Dropping ...\n"),
                 tcp_hdr->header_len);
        return result_ok();
    }

    sz hdr_bytes = (sz) tcp_hdr->header_len * 4;
    if (hdr_bytes > segment.len) {
        pr_debug(STR("TCP header length %hhd (%ld bytes) exceeds segment "
                     "size %ld. Dropping ...\n"),
                 tcp_hdr->header_len, hdr_bytes, segment.len);
        return result_ok();
    }
    if (!tcp_checksum_is_ok(pseudo_hdr, segment)) {
        pr_debug(
            STR("Received TCP segment with invalid checksum. Dropping ...\n"));
        return result_ok();
    }

    struct byte_view payload = byte_view_skip(segment, hdr_bytes);
    if (payload.len < 0 || payload.dat == NULL) {
        pr_debug(STR("Invalid TCP payload after header parse. Dropping ...\n"));
        return result_ok();
    }

    struct ipv4_addr host_addr = pseudo_hdr.dest_addr;
    struct ipv4_addr peer_addr = pseudo_hdr.src_addr;
    u16 host_port = u16_from_net_u16(tcp_hdr->dest_port);
    u16 peer_port = u16_from_net_u16(tcp_hdr->src_port);
    if (ipv4_addr_is_equal(peer_addr, IPV4_ADDR_ANY)) {
        pr_debug(
            STR("Received TCP segment with spoofed source 0.0.0.0. "
                "Dropping ...\n"));
        return result_ok();
    }

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    struct result ret =
        tcp_handle_packet_locked(segment, tcp_hdr, payload, host_addr,
                                 peer_addr, host_port, peer_port, sb, tmp);
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

/* User interface */

struct tcp_conn *tcp_conn_listen(struct ipv4_addr addr,
                                 u16 port,
                                 struct arena tmp)
{
    assert(global_tcp_is_initialized);

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);

    struct tcp_conn *conn = tcp_lookup_conn(addr, IPV4_ADDR_ANY, port, 0);

    if (conn && conn->state == TCP_CONN_STATE_LISTEN) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        return conn;
    }

    conn = tcp_conn_alloc_and_init(addr, port, TCP_CONN_DEFAULT_MSS,
                                   TCP_CONN_STATE_LISTEN);
    if (!conn) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        pr_err(STR("TCP: Failed to allocate LISTEN connection for %s:%hu: "
                   "err=%d\n"),
               ipv4_addr_format(addr, &tmp), port, ENOMEM);
        return NULL;
    }

    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    pr_info(STR("New connection in LISTEN state on %s:%hu ...\n"),
            ipv4_addr_format(addr, &tmp), port);

    return conn;
}

struct tcp_conn *tcp_conn_connect(struct ipv4_addr remote_addr,
                                  u16 remote_port,
                                  struct arena tmp)
{
    assert(global_tcp_is_initialized);

    /* Determine the local IP address for this destination. */
    struct result_ipv4_addr local_res = tcp_net_ops->ip_route_addr(remote_addr);
    if (local_res.is_error) {
        pr_err(STR("TCP connect: no route to %s\n"),
               ipv4_addr_format(remote_addr, &tmp));
        return NULL;
    }
    struct ipv4_addr local_addr = result_ipv4_addr_checked(local_res);

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);

    u16 local_port = tcp_alloc_ephemeral_port();
    if (local_port == 0) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        pr_err(STR("TCP connect: ephemeral port exhaustion\n"));
        return NULL;
    }

    struct tcp_conn *conn = tcp_conn_alloc_and_init(
        local_addr, local_port, TCP_CONN_DEFAULT_MSS, TCP_CONN_STATE_SYN_SENT);
    if (!conn) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        pr_err(STR("TCP connect: connection pool full\n"));
        return NULL;
    }

    conn->peer_addr = remote_addr;
    conn->peer_port = remote_port;
    conn->use_window_scale = true;

    tcp_hash_insert(conn);

    /* SYN_SENT timeout: transition to RESET if no SYN-ACK in time. */
    conn->internal_timeout =
        time_ms_new(tcp_net_ops->current_ms().ms + TCP_CONN_SYN_SENT_TIMEOUT);

    /* Send the initial SYN.  send_next is advanced by tcp_send_segment_empty
     * (SYN consumes one sequence number).
     */
    struct result syn_res = tcp_send_segment_empty(conn, TCP_HDR_FLAG_SYN, tmp);
    if (syn_res.is_error) {
        tcp_hash_remove(conn);
        tcp_free_conn(conn);
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        pr_err(STR("TCP connect: failed to send SYN\n"));
        return NULL;
    }

    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    pr_info(STR("TCP connect: SYN sent to %s:%hu from %s:%hu\n"),
            ipv4_addr_format(remote_addr, &tmp), remote_port,
            ipv4_addr_format(local_addr, &tmp), local_port);
    return conn;
}

bool tcp_conn_is_connected(struct tcp_conn *conn)
{
    assert(global_tcp_is_initialized);
    assert(conn);
    u64 flags = spin_lock_irqsave(&conn->lock);
    bool ret = conn->state == TCP_CONN_STATE_ESTABLISHED;
    spin_unlock_irqrestore(&conn->lock, flags);
    return ret;
}

bool tcp_conn_is_reset(struct tcp_conn *conn)
{
    assert(global_tcp_is_initialized);
    assert(conn);
    u64 flags = spin_lock_irqsave(&conn->lock);
    bool ret = conn->state == TCP_CONN_STATE_RESET;
    spin_unlock_irqrestore(&conn->lock, flags);
    return ret;
}

struct tcp_conn *tcp_conn_accept(struct tcp_conn *listen_conn)
{
    assert(global_tcp_is_initialized);
    assert(listen_conn);

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);

    struct tcp_conn *conn = __container_of(listen_conn->accept_queue.next,
                                           struct tcp_conn, accept_queue);
    assert(conn);

    if (conn == listen_conn) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        return NULL;
    }

    /* Depending on the timing, the user may call accept when a SYN has been
     * received but before the handshake was completed. In that case the
     * connection is in the SYN_RCVD state, but it's not ready to receive data.
     */
    if (!tcp_conn_needs_user_close(conn)) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        return NULL;
    }

    list_del_init(&conn->accept_queue);
    u64 listen_flags = spin_lock_irqsave(&listen_conn->lock);
    if (listen_conn->accept_ready > 0)
        listen_conn->accept_ready--;
    spin_unlock_irqrestore(&listen_conn->lock, listen_flags);

    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return conn;
}

bool tcp_conn_has_data(struct tcp_conn *conn)
{
    assert(global_tcp_is_initialized);
    assert(conn);
    /* recv_buf head/tail are mutated under tcp_pool_lock in the RX path,
     * so we must hold the same lock for a consistent read.
     */
    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    bool ret = recv_buf_count(conn->recv_buf) != 0;
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

void tcp_conn_set_notify(struct tcp_conn *conn, const struct tcp_notify *notify)
{
    assert(global_tcp_is_initialized);
    assert(conn);
    /* Serialize with tcp_notify_get() which snapshots under conn->lock. */
    u64 flags = spin_lock_irqsave(&conn->lock);
    conn->notify = notify;
    spin_unlock_irqrestore(&conn->lock, flags);
}

bool tcp_conn_has_pending(struct tcp_conn *listen_conn)
{
    assert(global_tcp_is_initialized);
    assert(listen_conn);

    /* Match the actual accept behavior: the queue head must be an established
     * connection that tcp_conn_accept would return, not a SYN_RCVD half-open
     * that would cause accept to return NULL.  This prevents callers from
     * spinning on accept when a stale handshake blocks the queue head.
     */
    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    bool ret = false;
    if (!list_empty(&listen_conn->accept_queue)) {
        struct tcp_conn *head = __container_of(listen_conn->accept_queue.next,
                                               struct tcp_conn, accept_queue);
        ret = (head != listen_conn) && tcp_conn_needs_user_close(head);
    }
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

static inline bool tcp_conn_closed_by_peer(enum tcp_conn_state state)
{
    return (state == TCP_CONN_STATE_CLOSE_WAIT) ||
           (state == TCP_CONN_STATE_RESET);
}

static inline sz tcp_send_window_avail(struct tcp_conn *conn)
{
    u32 send_next = conn->send_next;
    u32 send_unack = conn->send_unack;

    /* Bytes in flight (wrapping-safe). */
    u32 in_flight = send_next - send_unack;

    /* Effective window = min(rwnd, cwnd) per RFC 5681 §3.1.
     * The receiver's advertised window (rwnd) limits how much the peer
     * can buffer; the congestion window (cwnd) limits how much is injected
     * into the network.
     */
    u32 rwnd = (conn->send_window_real > U32_MAX)
                   ? U32_MAX
                   : (u32) conn->send_window_real;
    u32 eff_window = (conn->cwnd < rwnd) ? conn->cwnd : rwnd;

    if (in_flight >= eff_window)
        return 0;
    return eff_window - in_flight;
}

static struct result_sz tcp_conn_send_locked(struct tcp_conn *conn,
                                             struct byte_view payload,
                                             bool *peer_closed_conn,
                                             sz ip_mtu,
                                             struct arena tmp)
{
    assert(conn);
    assert(peer_closed_conn);

    *peer_closed_conn = tcp_conn_closed_by_peer(conn->state);
    sz len = MAX(0, ip_mtu - sizeof(struct tcp_header));
    len = MIN(len, conn->mss);
    len = MIN(len, payload.len);
    if (len == 0)
        return result_sz_ok(0);

    sz n_sent = 0;

    for (sz i = 0; i < payload.len; i += len) {
        if (tcp_send_window_avail(conn) < MIN(payload.len - i, len))
            return result_sz_ok(n_sent);

        struct byte_view fragment =
            byte_view_new(payload.dat + i, MIN(payload.len - i, len));
        struct result res =
            tcp_send_segment(conn, TCP_HDR_FLAG_ACK, fragment, tmp);
        if (res.is_error)
            return result_sz_error(res.code);

        n_sent += fragment.len;
        __atomic_fetch_add(&global_tcp_stats_bytes_tx, fragment.len,
                           __ATOMIC_RELAXED);
    }

    return result_sz_ok(n_sent);
}

struct result_sz tcp_conn_send(struct tcp_conn *conn,
                               struct byte_view payload,
                               bool *peer_closed_conn,
                               struct arena tmp)
{
    assert(global_tcp_is_initialized);
    assert(conn);

    /* peer_addr is immutable after connection setup — no lock needed. */
    struct result_sz ip_mtu_res = tcp_net_ops->ip_route_mtu(conn->peer_addr);
    if (ip_mtu_res.is_error)
        return result_sz_error(ip_mtu_res.code);

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    struct result_sz ret = tcp_conn_send_locked(
        conn, payload, peer_closed_conn, result_sz_checked(ip_mtu_res), tmp);
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

struct result_sz tcp_conn_recv(struct tcp_conn *conn,
                               struct byte_buf *buf,
                               bool *peer_closed_conn)
{
    assert(global_tcp_is_initialized);
    assert(conn);
    assert(buf);
    assert(peer_closed_conn);

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);

    *peer_closed_conn = tcp_conn_closed_by_peer(conn->state);

    sz avail = recv_buf_count(conn->recv_buf);
    if (!avail) {
        spin_unlock_irqrestore(&tcp_pool_lock, flags);
        return result_sz_ok(0);
    }

    sz n_read = recv_buf_read(&conn->recv_buf, buf);

    __atomic_fetch_add(&global_tcp_stats_bytes_rx, n_read, __ATOMIC_RELAXED);

    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return result_sz_ok(n_read);
}

static struct result tcp_conn_close_locked(struct tcp_conn **conn_ptr,
                                           struct arena tmp)
{
    assert(conn_ptr);
    assert(*conn_ptr);

    struct tcp_conn *conn = *conn_ptr;
    *conn_ptr = NULL;

    if (conn->state == TCP_CONN_STATE_LISTEN ||
        conn->state == TCP_CONN_STATE_SYN_SENT ||
        conn->state == TCP_CONN_STATE_SYN_RCVD ||
        conn->state == TCP_CONN_STATE_RESET) {
        tcp_free_conn(conn);
        return result_ok();
    }

    if (conn->state == TCP_CONN_STATE_ESTABLISHED) {
        tcp_set_state(conn, TCP_CONN_STATE_FIN_WAIT_1);

        tcp_conn_set_internal_timeout(conn);

        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_FIN | TCP_HDR_FLAG_ACK,
                                      tmp);
    }

    if (conn->state == TCP_CONN_STATE_CLOSE_WAIT) {
        tcp_set_state(conn, TCP_CONN_STATE_LAST_ACK);

        tcp_conn_set_internal_timeout(conn);

        return tcp_send_segment_empty(conn, TCP_HDR_FLAG_FIN | TCP_HDR_FLAG_ACK,
                                      tmp);
    }

    return result_ok();
}

struct result tcp_conn_close(struct tcp_conn **conn_ptr, struct arena tmp)
{
    assert(global_tcp_is_initialized);
    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    struct result ret = tcp_conn_close_locked(conn_ptr, tmp);
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

/* TCP statistics */

struct tcp_stats tcp_stats_get(void)
{
    return (struct tcp_stats) {
        .uptime = tcp_net_ops->current_ms(),
        .n_connections =
            __atomic_load_n(&global_tcp_stats_n_connections, __ATOMIC_ACQUIRE),
        .sbq_mem = __atomic_load_n(&global_tcp_stats_sbq_mem, __ATOMIC_RELAXED),
        .recv_mem =
            __atomic_load_n(&global_tcp_stats_recv_mem, __ATOMIC_RELAXED),
        .bytes_tx =
            __atomic_load_n(&global_tcp_stats_bytes_tx, __ATOMIC_RELAXED),
        .bytes_rx =
            __atomic_load_n(&global_tcp_stats_bytes_rx, __ATOMIC_RELAXED),
        .retransmits =
            __atomic_load_n(&global_tcp_stats_retransmits, __ATOMIC_RELAXED),
        .pool_exhaustion = __atomic_load_n(&global_tcp_stats_pool_exhaustion,
                                           __ATOMIC_RELAXED),
    };
}

static struct str tcp_conn_state_name(enum tcp_conn_state state)
{
    switch (state) {
    case TCP_CONN_STATE_LISTEN:
        return STR("LISTEN");
    case TCP_CONN_STATE_ESTABLISHED:
        return STR("ESTABLISHED");
    case TCP_CONN_STATE_CLOSE_WAIT:
        return STR("CLOSE_WAIT");
    case TCP_CONN_STATE_RESET:
        return STR("RESET");
    case TCP_CONN_STATE_SYN_SENT:
        return STR("SYN_SENT");
    case TCP_CONN_STATE_SYN_RCVD:
        return STR("SYN_RCVD");
    case TCP_CONN_STATE_LAST_ACK:
        return STR("LAST_ACK");
    case TCP_CONN_STATE_FIN_WAIT_1:
        return STR("FIN_WAIT_1");
    case TCP_CONN_STATE_FIN_WAIT_2:
        return STR("FIN_WAIT_2");
    case TCP_CONN_STATE_CLOSING:
        return STR("CLOSING");
    case TCP_CONN_STATE_TIME_WAIT:
        return STR("TIME_WAIT");
    default:
        return STR("UNKNOWN");
    }
}

void tcp_for_each_conn(tcp_conn_iter_cb_t cb, void *ctx)
{
    /* Snapshot connection info under the lock, then invoke the callback
     * without holding it.  This avoids blocking packet processing during
     * potentially expensive JSON formatting in the /api/tcp handler.
     */
    struct tcp_conn_info snapshot[TCP_CONN_MAX_NUM];
    sz n = 0;

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *conn = &global_tcp_conn_table[i];
        if (!conn->is_used)
            continue;
        snapshot[n++] = (struct tcp_conn_info) {
            .host_addr = conn->host_addr,
            .peer_addr = conn->peer_addr,
            .host_port = conn->host_port,
            .peer_port = conn->peer_port,
            .state_name = tcp_conn_state_name(conn->state),
            .send_next = conn->send_next,
            .recv_next = conn->recv_next,
            .cwnd = conn->cwnd,
            .ssthresh = conn->ssthresh,
            .pkts_sent = conn->stats_pkts_sent,
            .pkts_recv = conn->stats_pkts_recv,
            .bytes_sent = conn->stats_bytes_sent,
            .bytes_recv = conn->stats_bytes_recv,
            .retransmits = conn->stats_retransmits,
            .dup_acks = conn->stats_dup_acks,
            .ooo_segments = conn->stats_ooo_segments,
        };
    }
    spin_unlock_irqrestore(&tcp_pool_lock, flags);

    for (sz i = 0; i < n; i++)
        cb(snapshot[i], ctx);
}

#if CONFIG_PMTUD
void tcp_pmtu_update(struct ipv4_addr dest, u16 new_mtu)
{
    assert(global_tcp_is_initialized);

    /* TCP MSS = MTU - IP header (20) - TCP header (20). */
    sz new_mss = (new_mtu > 40) ? (sz) (new_mtu - 40) : 1;

    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        struct tcp_conn *conn = &global_tcp_conn_table[i];
        if (!conn->is_used)
            continue;
        if (!ipv4_addr_is_equal(conn->peer_addr, dest))
            continue;
        if (conn->mss > new_mss)
            conn->mss = new_mss;
    }
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
}
#endif /* CONFIG_PMTUD */

/* SACK stubs -- compile-time placeholders for CONFIG_TCP_SACK.
 * These satisfy the test harness (tests/tests-tcp.c) so that an
 * allyesconfig builds.  Every stub test returns failure until the
 * real implementation lands.
 */
#if CONFIG_TCP_SACK

struct tcp_option_sack_permitted {
    u8 nop1;
    u8 nop2;
    u8 kind;
    u8 length;
} __packed;

#endif /* CONFIG_TCP_SACK */

/* SYN-cookie stubs -- compile-time placeholders for CONFIG_SYN_COOKIES. */
#if CONFIG_SYN_COOKIES

static const u16 tcp_syncookie_mss_table[] = {
    536, 1220, 1360, 1440, 1452, 1460, 1480, 1500,
};

static u32 tcp_syncookie_encode_mss(u16 mss)
{
    /* Return the index of the closest table entry. */
    u32 best = 0;
    for (u32 i = 1; i < countof(tcp_syncookie_mss_table); i++) {
        u16 d_cur = (mss > tcp_syncookie_mss_table[i])
                        ? (u16) (mss - tcp_syncookie_mss_table[i])
                        : (u16) (tcp_syncookie_mss_table[i] - mss);
        u16 d_best = (mss > tcp_syncookie_mss_table[best])
                         ? (u16) (mss - tcp_syncookie_mss_table[best])
                         : (u16) (tcp_syncookie_mss_table[best] - mss);
        if (d_cur < d_best)
            best = i;
    }
    return best;
}

/* Simple hash of the 4-tuple for cookie generation.  Uses a Jenkins
 * one-at-a-time hash seeded with a compile-time secret.  The low 3 bits
 * encode the MSS table index; the remaining bits are the hash.
 */
static u32 syncookie_hash(struct ipv4_addr la,
                          u16 lp,
                          struct ipv4_addr ra,
                          u16 rp)
{
    u32 h = 0x5A17C00E; /* arbitrary seed */
    u8 key[12];
    key[0] = la.addr[0];
    key[1] = la.addr[1];
    key[2] = la.addr[2];
    key[3] = la.addr[3];
    key[4] = ra.addr[0];
    key[5] = ra.addr[1];
    key[6] = ra.addr[2];
    key[7] = ra.addr[3];
    key[8] = (u8) (lp >> 8);
    key[9] = (u8) lp;
    key[10] = (u8) (rp >> 8);
    key[11] = (u8) rp;
    for (u32 i = 0; i < 12; i++) {
        h += key[i];
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

static u32 tcp_syncookie_generate(struct ipv4_addr local_addr,
                                  u16 local_port,
                                  struct ipv4_addr remote_addr,
                                  u16 remote_port,
                                  u16 mss)
{
    u32 mss_idx = tcp_syncookie_encode_mss(mss);
    u32 h = syncookie_hash(local_addr, local_port, remote_addr, remote_port);
    return (h & ~(u32) 7) | (mss_idx & 7);
}

static u16 tcp_syncookie_validate(struct ipv4_addr local_addr,
                                  u16 local_port,
                                  struct ipv4_addr remote_addr,
                                  u16 remote_port,
                                  u32 cookie)
{
    u32 h = syncookie_hash(local_addr, local_port, remote_addr, remote_port);
    if ((cookie & ~(u32) 7) != (h & ~(u32) 7))
        return 0;
    u32 idx = cookie & 7;
    if (idx >= countof(tcp_syncookie_mss_table))
        return 0;
    return tcp_syncookie_mss_table[idx];
}

#endif /* CONFIG_SYN_COOKIES */

/* Self-tests */

#include __INC_TEST(tcp)
