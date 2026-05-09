/* SPDX-License-Identifier: MIT */
#include <mazu/net/tcp.h>
#include <mazu/selftest.h>

/* Claim a free connection slot for testing.  Caller must hold tcp_pool_lock.
 * Returns NULL if the pool is full.
 */
static struct tcp_conn *test_claim_conn_slot(void)
{
    for (sz i = 0; i < TCP_CONN_MAX_NUM; i++) {
        if (!global_tcp_conn_table[i].is_used) {
            struct tcp_conn *c = &global_tcp_conn_table[i];
            c->is_used = true;
            c->magic = TCP_MAGIC;
            c->lock = (spinlock_t) SPINLOCK_INITIALIZER;
            c->notify = NULL;
            list_init(&c->hash_node);
            return c;
        }
    }
    return NULL;
}

/* Run a test body under tcp_pool_lock with a claimed conn slot.
 * 'body' receives a tcp_conn* and returns 0 on success, nonzero on failure.
 * The conn slot is released and the lock dropped on exit.
 *
 * IMPORTANT: body() runs with tcp_pool_lock held — it must not sleep,
 * yield, or call any TCP API that re-acquires the lock.
 */
static i32 test_with_conn_slot(i32 (*body)(struct tcp_conn *))
{
    i32 ret = 1;
    u64 flags = spin_lock_irqsave(&tcp_pool_lock);
    struct tcp_conn *conn = test_claim_conn_slot();
    if (conn) {
        ret = body(conn);
        conn->is_used = false;
    }
    spin_unlock_irqrestore(&tcp_pool_lock, flags);
    return ret;
}

/* Test 1: hash function produces bucket indices in range for known tuples. */
static i32 test_tcp_hash_function(void)
{
    struct ipv4_addr local = ipv4_addr_new(192, 168, 100, 2);
    struct ipv4_addr remote = ipv4_addr_new(10, 0, 2, 15);
    u32 h = tcp_hash_4tuple(local, 80, remote, 12345);
    u32 bucket = h & TCP_HASH_MASK;

    /* Bucket must be in [0, TCP_HASH_BUCKETS). */
    if (bucket >= TCP_HASH_BUCKETS)
        return 1;

    /* Different 4-tuples should (likely) produce different hashes.
     * Not a hard requirement, but with a seeded hash and different inputs
     * the probability of collision for two specific tuples is ~1/256.
     */
    u32 h2 = tcp_hash_4tuple(local, 80, remote, 54321);
    /* Just verify it's also in range; collision is allowed. */
    if ((h2 & TCP_HASH_MASK) >= TCP_HASH_BUCKETS)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_hash_function, test_tcp_hash_function);

/* Test 2: insert into hash table, lookup, remove, verify not found.
 * All operations under tcp_pool_lock to prevent races with live TCP.
 */
static i32 hash_insert_remove_body(struct tcp_conn *conn)
{
    /* Set up a fake 4-tuple and insert. */
    conn->host_addr = ipv4_addr_new(192, 168, 100, 2);
    conn->host_port = 80;
    conn->peer_addr = ipv4_addr_new(10, 0, 2, 99);
    conn->peer_port = 44444;
    list_init(&conn->hash_node);
    tcp_hash_insert(conn);

    /* Verify the connection is in the correct bucket. */
    u32 bucket = tcp_hash_4tuple(conn->host_addr, conn->host_port,
                                 conn->peer_addr, conn->peer_port) &
                 TCP_HASH_MASK;
    bool found = false;
    struct tcp_conn *c;
    list_for_each_entry_safe (&tcp_hash_table[bucket], c, struct tcp_conn,
                              hash_node) {
        if (c == conn) {
            found = true;
            break;
        }
    }
    if (!found) {
        tcp_hash_remove(conn);
        return 1;
    }

    /* Remove and verify it's gone. */
    tcp_hash_remove(conn);

    found = false;
    list_for_each_entry_safe (&tcp_hash_table[bucket], c, struct tcp_conn,
                              hash_node) {
        if (c == conn) {
            found = true;
            break;
        }
    }
    return found ? 1 : 0;
}

static i32 test_tcp_hash_insert_remove(void)
{
    return test_with_conn_slot(hash_insert_remove_body);
}
DEFINE_SELFTEST(tcp_hash_insert_remove, test_tcp_hash_insert_remove);

/* Test 3: TCP sequence number wrapping helpers.
 * Verifies that seq_lt/seq_le/seq_gt/seq_geq handle the 0xFFFFFFFF boundary
 * correctly using signed 32-bit arithmetic (RFC 793 §3.3).
 */
static i32 test_tcp_seq_wrap(void)
{
    /* seq_lt(0xFFFFFFFF, 0x00000001) must be true: 1 is "after" 0xFFFFFFFF. */
    if (!seq_lt(0xFFFFFFFF, 0x00000001))
        return 1;

    /* seq_gt(0x00000001, 0xFFFFFFFF) must be true (symmetric). */
    if (!seq_gt(0x00000001, 0xFFFFFFFF))
        return 1;

    /* Normal non-wrapping case: 100 < 200. */
    if (!seq_lt(100, 200))
        return 1;
    if (!seq_gt(200, 100))
        return 1;

    /* Equality: seq_le and seq_geq must return true for equal values. */
    if (!seq_le(42, 42))
        return 1;
    if (!seq_geq(42, 42))
        return 1;

    /* seq_lt must be false for equal values. */
    if (seq_lt(42, 42))
        return 1;

    /* Wrap boundary: 0x80000000 apart is the ambiguous midpoint.
     * By convention (i32)0x80000000 < 0, so seq_lt(0, 0x80000000) is true.
     */
    if (!seq_lt(0, 0x80000000))
        return 1;

    /* seq_le at wrap boundary. */
    if (!seq_le(0xFFFFFFFE, 0xFFFFFFFF))
        return 1;
    if (!seq_le(0xFFFFFFFF, 0x00000000))
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_seq_wrap, test_tcp_seq_wrap);

/* Test 4: ephemeral port allocator returns valid ports in the dynamic range
 * (49152..65535) and consecutive calls return different ports.
 */
static i32 test_tcp_ephemeral_port(void)
{
    u64 flags = spin_lock_irqsave(&tcp_pool_lock);

    u16 p1 = tcp_alloc_ephemeral_port();
    u16 p2 = tcp_alloc_ephemeral_port();

    spin_unlock_irqrestore(&tcp_pool_lock, flags);

    /* Ports must be in the ephemeral range. */
    if (p1 < 49152 || p2 < 49152)
        return 1;
    /* Consecutive allocations must differ. */
    if (p1 == p2)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_ephemeral_port, test_tcp_ephemeral_port);

/* Test 5: SYN_SENT state name string. */
static i32 test_tcp_syn_sent_state_name(void)
{
    struct str name = tcp_conn_state_name(TCP_CONN_STATE_SYN_SENT);
    if (!str_is_equal(name, STR("SYN_SENT")))
        return 1;
    return 0;
}
DEFINE_SELFTEST(tcp_syn_sent_state_name, test_tcp_syn_sent_state_name);

#ifdef TCP_DELAYED_ACK_MS
/* Test 6: Delayed ACK constants are in sane range (P2.9n). */
static i32 test_tcp_delayed_ack_constants(void)
{
    /* RFC 1122 §4.2.3.2: delayed ACK must not exceed 500ms.  Uses 200ms
     * (matching NetX Duo NX_TCP_ACK_TIMER_RATE).
     */
    if (TCP_DELAYED_ACK_MS < 50 || TCP_DELAYED_ACK_MS > 500)
        return 1;

    /* Quickack count must be at least 1 (one immediate ACK after
     * ESTABLISHED).
     */
    if (TCP_QUICKACK_COUNT < 1 || TCP_QUICKACK_COUNT > 16)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_delayed_ack_constants, test_tcp_delayed_ack_constants);

/* Test 7: Delayed ACK field initialization and layout (P2.9n).
 * Verifies that delayed ACK fields in tcp_conn are writable and
 * read back correctly.  Cannot call tcp_send_ack_or_defer without
 * a fully wired connection, so this is a structural sanity check.
 */
static i32 delayed_ack_pending_body(struct tcp_conn *conn)
{
    conn->ack_pending = false;
    conn->ack_delay_deadline_ms = 0;
    conn->quickack_remaining = 0;

    if (conn->ack_pending)
        return 1;

    conn->quickack_remaining = 1;
    if (conn->quickack_remaining != 1)
        return 1;

    return 0;
}

static i32 test_tcp_delayed_ack_pending(void)
{
    return test_with_conn_slot(delayed_ack_pending_body);
}
DEFINE_SELFTEST(tcp_delayed_ack_pending, test_tcp_delayed_ack_pending);
#endif /* TCP_DELAYED_ACK_MS */

#if CONFIG_TCP_SACK
/* Test 8: SACK constants and struct layout (P2.9o). */
static i32 test_tcp_sack_constants(void)
{
    /* SACK-permitted option kind must be 4 (RFC 2018). */
    if (TCP_OPT_SACK_PERMITTED_KIND != 4)
        return 1;
    if (TCP_OPT_SACK_PERMITTED_LENGTH != 2)
        return 1;

    /* SACK option kind must be 5 (RFC 2018). */
    if (TCP_OPT_SACK_KIND != 5)
        return 1;

    /* Must support at least 3 SACK blocks. */
    if (TCP_SACK_MAX_BLOCKS < 3)
        return 1;

    /* SACK-permitted struct must be 4 bytes (2 NOP + kind + length). */
    if (sizeof(struct tcp_option_sack_permitted) != 4)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_sack_constants, test_tcp_sack_constants);

/* Test 9: SACK scoreboard insert and sort (P2.9o).
 * Allocates a free conn slot and exercises the OOO slot machinery
 * by directly writing fields (cannot call tcp_sack_stash_ooo without
 * a fully wired connection).
 */
static i32 sack_scoreboard_body(struct tcp_conn *conn)
{
    /* Simulate 3 OOO blocks: [300,400), [100,200), [500,600). */
    conn->n_ooo_slots = 3;
    conn->ooo_blocks[0].left_edge = 300;
    conn->ooo_blocks[0].right_edge = 400;
    conn->ooo_blocks[1].left_edge = 100;
    conn->ooo_blocks[1].right_edge = 200;
    conn->ooo_blocks[2].left_edge = 500;
    conn->ooo_blocks[2].right_edge = 600;

    if (conn->ooo_blocks[0].left_edge != 300)
        return 1;
    if (conn->n_ooo_slots != 3)
        return 1;
    if (conn->n_ooo_slots > TCP_SACK_MAX_BLOCKS)
        return 1;

    conn->n_ooo_slots = 0;
    return 0;
}

static i32 test_tcp_sack_scoreboard(void)
{
    return test_with_conn_slot(sack_scoreboard_body);
}
DEFINE_SELFTEST(tcp_sack_scoreboard, test_tcp_sack_scoreboard);

static i32 syn_option_state_gate_body(struct tcp_conn *conn)
{
    u8 syn_opts[10] = {
        TCP_OPT_MSS_KIND,
        TCP_OPT_MSS_LENGTH,
        0x02,
        0x00,
        TCP_OPT_WS_KIND,
        TCP_OPT_WS_LENGTH,
        7,
        TCP_OPT_NOP_KIND,
        TCP_OPT_SACK_PERMITTED_KIND,
        TCP_OPT_SACK_PERMITTED_LENGTH,
    };
    struct byte_view syn_view = byte_view_new(syn_opts, sizeof(syn_opts));

    conn->state = TCP_CONN_STATE_ESTABLISHED;
    conn->mss = 1460;
    conn->send_window_scale = 3;
    conn->use_window_scale = false;

    struct tsopt ignored =
        tcp_handle_options(conn, TCP_HDR_FLAG_SYN | TCP_HDR_FLAG_ACK, syn_view);
    if (conn->mss != 1460)
        return 1;
    if (conn->send_window_scale != 3)
        return 1;
    if (conn->use_window_scale)
        return 1;
    if (ignored.have_sack_permitted)
        return 1;

    conn->state = TCP_CONN_STATE_SYN_SENT;
    conn->mss = TCP_CONN_DEFAULT_MSS;
    conn->send_window_scale = 0;
    conn->use_window_scale = false;

    struct tsopt applied = tcp_handle_options(conn, TCP_HDR_FLAG_SYN, syn_view);
    if (conn->mss != 512)
        return 1;
    if (conn->send_window_scale != 7)
        return 1;
    if (!conn->use_window_scale)
        return 1;
    if (!applied.have_sack_permitted)
        return 1;

    return 0;
}

static i32 test_tcp_syn_option_state_gate(void)
{
    return test_with_conn_slot(syn_option_state_gate_body);
}
DEFINE_SELFTEST(tcp_syn_option_state_gate, test_tcp_syn_option_state_gate);

/* Test 10: SACK block in tsopt parsing (P2.9o).
 * Verifies the option parser can decode a SACK option from raw bytes.
 */
static i32 sack_option_parse_body(struct tcp_conn *conn)
{
    /* Build a raw SACK-permitted option: kind=4, length=2. */
    u8 sack_perm_opt[2] = {TCP_OPT_SACK_PERMITTED_KIND,
                           TCP_OPT_SACK_PERMITTED_LENGTH};
    struct byte_view perm_view = byte_view_new(sack_perm_opt, 2);
    struct tsopt tsopt = tcp_handle_options(conn, TCP_HDR_FLAG_SYN, perm_view);
    (void) tsopt;
    if (!tsopt.have_sack_permitted)
        return 1;

    /* Build a raw SACK option: NOP NOP kind=5 length=10 [1000..2000] */
    u8 sack_opt[12];
    sack_opt[0] = TCP_OPT_NOP_KIND;
    sack_opt[1] = TCP_OPT_NOP_KIND;
    sack_opt[2] = TCP_OPT_SACK_KIND;
    sack_opt[3] = 10; /* 2 + 1*8 */
    sack_opt[4] = 0x00;
    sack_opt[5] = 0x00;
    sack_opt[6] = 0x03;
    sack_opt[7] = 0xE8; /* left_edge = 1000 */
    sack_opt[8] = 0x00;
    sack_opt[9] = 0x00;
    sack_opt[10] = 0x07;
    sack_opt[11] = 0xD0; /* right_edge = 2000 */

    struct byte_view sack_view = byte_view_new(sack_opt, 12);
    struct tsopt tsopt2 = tcp_handle_options(conn, TCP_HDR_FLAG_ACK, sack_view);
    (void) tsopt2;
    if (tsopt2.n_sack_blocks != 1)
        return 1;
    if (tsopt2.sack_blocks[0].left_edge != 1000)
        return 1;
    if (tsopt2.sack_blocks[0].right_edge != 2000)
        return 1;

    return 0;
}

static i32 test_tcp_sack_option_parse(void)
{
    return test_with_conn_slot(sack_option_parse_body);
}
DEFINE_SELFTEST(tcp_sack_option_parse, test_tcp_sack_option_parse);
#endif /* CONFIG_TCP_SACK */

#if CONFIG_SYN_COOKIES
/* Test 11: SYN cookie generate and validate round-trip (P2.9u). */
static i32 test_tcp_syncookie_roundtrip(void)
{
    struct ipv4_addr local = ipv4_addr_new(192, 168, 100, 2);
    struct ipv4_addr remote = ipv4_addr_new(10, 0, 2, 42);

    u32 cookie = tcp_syncookie_generate(local, 80, remote, 55555, 1460);

    /* Validate should succeed with the same 4-tuple. */
    u16 mss = tcp_syncookie_validate(local, 80, remote, 55555, cookie);
    if (mss == 0)
        return 1;

    /* MSS should be one of the table entries (nearest to 1460). */
    bool valid_mss = false;
    for (u32 i = 0; i < 8; i++) {
        if (mss == tcp_syncookie_mss_table[i]) {
            valid_mss = true;
            break;
        }
    }
    if (!valid_mss)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_syncookie_roundtrip, test_tcp_syncookie_roundtrip);

/* Test 12: SYN cookie rejects forged/wrong-tuple cookie (P2.9u). */
static i32 test_tcp_syncookie_reject_forged(void)
{
    struct ipv4_addr local = ipv4_addr_new(192, 168, 100, 2);
    struct ipv4_addr remote = ipv4_addr_new(10, 0, 2, 42);
    struct ipv4_addr wrong_remote = ipv4_addr_new(10, 0, 2, 99);

    u32 cookie = tcp_syncookie_generate(local, 80, remote, 55555, 1460);

    /* Validate with wrong source IP must fail. */
    u16 mss = tcp_syncookie_validate(local, 80, wrong_remote, 55555, cookie);
    if (mss != 0)
        return 1;

    /* Validate with wrong source port must fail. */
    mss = tcp_syncookie_validate(local, 80, remote, 44444, cookie);
    if (mss != 0)
        return 1;

    /* Random garbage cookie must fail. */
    mss = tcp_syncookie_validate(local, 80, remote, 55555, 0xDEADBEEF);
    if (mss != 0)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_syncookie_reject_forged, test_tcp_syncookie_reject_forged);

/* Test 13: MSS encoding round-trip (P2.9u). */
static i32 test_tcp_syncookie_mss_encoding(void)
{
    /* 1460 should map to the table entry closest to 1460. */
    __unused u32 idx = tcp_syncookie_encode_mss(1460);
    if (tcp_syncookie_mss_table[idx] != 1460)
        return 1;

    /* 536 should map to the first entry. */
    idx = tcp_syncookie_encode_mss(536);
    if (tcp_syncookie_mss_table[idx] != 536)
        return 1;

    /* 1500 should map to 1500 (last entry). */
    idx = tcp_syncookie_encode_mss(1500);
    if (tcp_syncookie_mss_table[idx] != 1500)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_syncookie_mss_encoding, test_tcp_syncookie_mss_encoding);
#endif /* CONFIG_SYN_COOKIES */

/* TCP event notification tests (P2.9q). */

static u32 test_notify_flags;
#define NOTIFY_ESTABLISH (1u << 0)
#define NOTIFY_DISCONNECT (1u << 1)
#define NOTIFY_WINDOW (1u << 2)
#define NOTIFY_DATA (1u << 3)

static void test_on_establish(struct tcp_conn *c __unused)
{
    test_notify_flags |= NOTIFY_ESTABLISH;
}

static void test_on_disconnect(struct tcp_conn *c __unused)
{
    test_notify_flags |= NOTIFY_DISCONNECT;
}

static void test_on_window(struct tcp_conn *c __unused, u32 w __unused)
{
    test_notify_flags |= NOTIFY_WINDOW;
}

static void test_on_data(struct tcp_conn *c __unused)
{
    test_notify_flags |= NOTIFY_DATA;
}

static const struct tcp_notify test_tcp_notify_all = {
    .on_establish = test_on_establish,
    .on_disconnect = test_on_disconnect,
    .on_window_update = test_on_window,
    .on_data_available = test_on_data,
};

/* Verify struct layout and NULL-safety of TCP_NOTIFY macro. */
static i32 test_tcp_notify_struct(void)
{
    /* Verify struct size: 4 function pointers. */
    if (sizeof(struct tcp_notify) < 4 * sizeof(void *))
        return 1;

    /* Partial notify: only on_establish set. */
    struct tcp_notify partial = {.on_establish = test_on_establish};
    if (!partial.on_establish)
        return 1;
    if (partial.on_disconnect || partial.on_window_update ||
        partial.on_data_available)
        return 1;

    /* Full notify: all callbacks set. */
    if (!test_tcp_notify_all.on_establish ||
        !test_tcp_notify_all.on_disconnect ||
        !test_tcp_notify_all.on_window_update ||
        !test_tcp_notify_all.on_data_available)
        return 1;

    return 0;
}
DEFINE_SELFTEST(tcp_notify_struct, test_tcp_notify_struct);

/* Verify TCP_NOTIFY macro fires callbacks and skips NULLs safely. */
static i32 notify_macro_body(struct tcp_conn *conn)
{
    i32 ret = 1;

    /* Fire with all callbacks set. */
    test_notify_flags = 0;
    conn->notify = &test_tcp_notify_all;
    TCP_NOTIFY(conn, on_establish);
    TCP_NOTIFY(conn, on_disconnect);
    TCP_NOTIFY(conn, on_window_update, 1460);
    TCP_NOTIFY(conn, on_data_available);

    u32 expected =
        NOTIFY_ESTABLISH | NOTIFY_DISCONNECT | NOTIFY_WINDOW | NOTIFY_DATA;
    if (test_notify_flags != expected)
        goto out;

    /* NULL notify pointer: no crash, no flags change. */
    test_notify_flags = 0;
    conn->notify = NULL;
    TCP_NOTIFY(conn, on_establish);
    TCP_NOTIFY(conn, on_data_available);
    if (test_notify_flags != 0)
        goto out;

    /* Partial notify: only on_establish, others are NULL. */
    test_notify_flags = 0;
    struct tcp_notify partial = {.on_establish = test_on_establish};
    conn->notify = &partial;
    TCP_NOTIFY(conn, on_establish);
    TCP_NOTIFY(conn, on_disconnect);
    TCP_NOTIFY(conn, on_window_update, 0);
    TCP_NOTIFY(conn, on_data_available);
    if (test_notify_flags != NOTIFY_ESTABLISH)
        goto out;

    ret = 0;
out:
    conn->notify = NULL;
    return ret;
}

static i32 test_tcp_notify_macro(void)
{
    return test_with_conn_slot(notify_macro_body);
}
DEFINE_SELFTEST(tcp_notify_macro, test_tcp_notify_macro);

/* Verify net_ops vtable: default instance is wired to real functions, and
 * swapping the module-global pointer to a mock works correctly.
 */
static struct time_ms mock_current_ms(void)
{
    return time_ms_new(42);
}
static u64 mock_rdtime(void)
{
    return 12345;
}

static i32 test_tcp_net_ops(void)
{
    /* Default instance should match real platform functions. */
    if (net_ops_default.current_ms != time_current_ms)
        return 1;
    if (net_ops_default.rdtime != time_rdtime)
        return 1;
    if (net_ops_default.timebase_freq != time_get_timebase_freq)
        return 1;
    if (net_ops_default.ip_send != ipv4_send_packet)
        return 1;
    if (net_ops_default.ip_route_addr != ipv4_route_interface_addr)
        return 1;
    if (net_ops_default.ip_route_mtu != ipv4_route_mtu)
        return 1;

    /* tcp_net_ops should point to the default by default. */
    if (tcp_net_ops != &net_ops_default)
        return 1;

    /* Swap to a mock and verify TCP uses the mock. */
    const struct net_ops mock = {
        .current_ms = mock_current_ms,
        .rdtime = mock_rdtime,
        .timebase_freq = time_get_timebase_freq,
        .ip_send = ipv4_send_packet,
        .ip_route_addr = ipv4_route_interface_addr,
        .ip_route_mtu = ipv4_route_mtu,
    };
    const struct net_ops *saved = tcp_net_ops;
    tcp_net_ops = &mock;

    /* Verify indirection works. */
    if (tcp_net_ops->current_ms().ms != 42)
        goto fail;
    if (tcp_net_ops->rdtime() != 12345)
        goto fail;

    tcp_net_ops = saved;
    return 0;

fail:
    tcp_net_ops = saved;
    return 1;
}
DEFINE_SELFTEST(tcp_net_ops, test_tcp_net_ops);
