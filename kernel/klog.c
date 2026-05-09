/* SPDX-License-Identifier: MIT */
/* Kernel log ring buffer (tee mode - no drain thread).
 *
 * klog_write captures printk output into a power-of-2 ring buffer while
 * the original console_write path provides immediate UART/semihosting
 * output.  The ring buffer is read by /api/klog (peek) and future
 * /dev/kmesg.  All ring operations are serialized by klog_lock (irqsave).
 */

#include <mazu/assert.h>
#include <mazu/fmt.h>
#include <mazu/klog.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>
#include <mazu/string.h>

static_assert((KLOG_SIZE & (KLOG_SIZE - 1)) == 0,
              "KLOG_SIZE must be power of "
              "2");

static char klog_buf[KLOG_SIZE];
static u64 klog_head; /* write position (monotonic, unsigned to avoid UB) */
static u64 klog_tail; /* read position (monotonic, unsigned to avoid UB) */
static u64 klog_dropped_bytes;
static spinlock_t klog_lock = SPINLOCK_INITIALIZER;

static bool klog_initialized;

/* Original console backend (UART or semihosting), saved by klog_init()
 * when installing the klog tee writer.  Used by klog_fault_event() and
 * klog_security_event() to output to UART without double-writing to the
 * klog ring (print_str routes through klog_tee_write after init).
 */
static console_write_fn_t original_console_write;

#define KLOG_MASK (KLOG_SIZE - 1)

void klog_fault_event(const char *kind,
                      u32 cpu,
                      u16 pid,
                      u16 tid,
                      u64 scause,
                      u64 sepc,
                      u64 stval,
                      u64 sstatus)
{
    if (!klog_initialized)
        return;

    sz kind_len = 0;
    if (kind) {
        while (kind[kind_len])
            kind_len++;
    }

    char line[192];
    struct str_buf sb = str_buf_new(line, 0, sizeof(line));
    fmt(&sb,
        STR("fault kind=%s cpu=%lu pid=%hu tid=%hu scause=%lx sepc=%lx "
            "stval=%lx sstatus=%lx\n"),
        str_new((char *) kind, kind_len), (u64) cpu, (u32) pid, (u32) tid,
        scause, sepc, stval, sstatus);
    klog_write(line, sb.len);
    if (original_console_write)
        original_console_write(str_from_buf(sb));
}

void klog_security_event(const char *action,
                         u16 pid,
                         u16 tid,
                         u64 syscall_nr,
                         u16 err_code)
{
    if (!klog_initialized)
        return;

    sz action_len = 0;
    if (action) {
        while (action_len < 32 && action[action_len])
            action_len++;
    }

    char line[128];
    struct str_buf sb = str_buf_new(line, 0, sizeof(line));
    fmt(&sb, STR("security action=%s pid=%hu tid=%hu syscall=%lu errno=%hu\n"),
        str_new((char *) action, action_len), (u32) pid, (u32) tid, syscall_nr,
        (u32) err_code);
    klog_write(line, sb.len);
    if (original_console_write)
        original_console_write(str_from_buf(sb));
}

void klog_write(const char *msg, sz len)
{
    if (!klog_initialized || !msg || len == 0)
        return;

    u64 flags = spin_lock_irqsave(&klog_lock);

    /* If the message is larger than the ring, skip the prefix that would
     * be overwritten anyway - only the last KLOG_SIZE bytes survive.
     */
    if (len > KLOG_SIZE) {
        klog_dropped_bytes += (klog_head - klog_tail) + (len - KLOG_SIZE);
        msg += len - KLOG_SIZE;
        len = KLOG_SIZE;
        klog_tail = klog_head; /* ring is empty before refill */
    }

    /* Drop oldest bytes to make room. */
    sz space = KLOG_SIZE - (klog_head - klog_tail);
    if (len > space) {
        sz drop = len - space;
        klog_tail += drop;
        klog_dropped_bytes += drop;
    }

    /* Copy in up to two chunks (ring wrap). */
    sz off = (sz) (klog_head & KLOG_MASK);
    sz first = KLOG_SIZE - off;
    if (first > len)
        first = len;
    memcpy(&klog_buf[off], msg, first);
    if (len > first)
        memcpy(&klog_buf[0], msg + first, len - first);
    klog_head += len;

    spin_unlock_irqrestore(&klog_lock, flags);
}

/* Copy n bytes from the ring at klog_tail into out (two-chunk memcpy). */
static void klog_ring_read(char *out, sz n)
{
    sz off = (sz) (klog_tail & KLOG_MASK);
    sz first = KLOG_SIZE - off;
    if (first > n)
        first = n;
    memcpy(out, &klog_buf[off], first);
    if (n > first)
        memcpy(out + first, &klog_buf[0], n - first);
}

sz klog_drain(char *out, sz max)
{
    u64 flags = spin_lock_irqsave(&klog_lock);

    sz avail = (sz) (klog_head - klog_tail);
    sz n = avail < max ? avail : max;
    klog_ring_read(out, n);
    klog_tail += n;

    spin_unlock_irqrestore(&klog_lock, flags);
    return n;
}

sz klog_peek(char *out, sz max)
{
    u64 flags = spin_lock_irqsave(&klog_lock);

    sz avail = (sz) (klog_head - klog_tail);
    sz n = avail < max ? avail : max;
    klog_ring_read(out, n);
    /* Do NOT advance tail; peek only. */

    spin_unlock_irqrestore(&klog_lock, flags);
    return n;
}

bool klog_has_data(void)
{
    /* Relaxed atomics prevent the compiler from caching across calls
     * (e.g. in a polling loop).  No lock needed - this is a hint.
     */
    u64 h = __atomic_load_n(&klog_head, __ATOMIC_RELAXED);
    u64 t = __atomic_load_n(&klog_tail, __ATOMIC_RELAXED);
    return h != t;
}

u64 klog_dropped(void)
{
    return __atomic_load_n(&klog_dropped_bytes, __ATOMIC_RELAXED);
}

/* Printk tee: writes to both the original console and the klog ring.
 * This preserves direct UART/semihosting output while also capturing
 * log data in the ring buffer for /api/klog.
 */
static struct result klog_tee_write(struct str s)
{
    klog_write(s.dat, s.len);
    return original_console_write(s);
}

void klog_init(void)
{
    assert(!klog_initialized);

    klog_head = 0;
    klog_tail = 0;
    klog_dropped_bytes = 0;

    klog_initialized = true;

    /* Tee printk output: messages go to both the klog ring buffer
     * and the original console (UART or semihosting).  The ring buffer
     * accumulates log data for /api/klog; the original path ensures
     * immediate console output is preserved.  No drain thread needed.
     */
    print_switch_to_klog(klog_tee_write, &original_console_write);
}

/* Self-tests */

#include __INC_TEST(klog)
