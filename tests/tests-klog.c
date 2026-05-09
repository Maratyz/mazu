/* SPDX-License-Identifier: MIT */
#include <mazu/klog.h>
#include <mazu/sched.h>
#include <mazu/selftest.h>
#include <mazu/time.h>

/* Test 1: write + peek.  In tee mode, printk output flows through the
 * ring constantly, so peek for the known marker string.
 */
static i32 test_klog_write_drain(void)
{
    /* Write a unique marker via klog_write (bypasses printk tee). */
    const char *marker = "KLOG_TEST_MARKER_XY";
    sz marker_len = 19;
    klog_write(marker, marker_len);

    /* Peek the full ring.  In tee mode, concurrent printk output may
     * push the marker further from the head - a smaller peek window
     * misses it under SMP timing variation.
     */
    static char out[KLOG_SIZE];
    sz n = klog_peek(out, sizeof(out));

    /* Search for marker in the peeked data. */
    for (sz i = 0; i + marker_len <= n; i++) {
        bool match = true;
        for (sz j = 0; j < marker_len; j++) {
            if (out[i + j] != marker[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return 0; /* found */
    }
    return 1; /* not found */
}
DEFINE_SELFTEST(klog_write_drain, test_klog_write_drain);

/* Test 2: overflow. Write more than KLOG_SIZE, verify dropped > 0. */
static i32 test_klog_overflow(void)
{
    u64 dropped_before = klog_dropped();

    /* Write KLOG_SIZE + 100 bytes to guarantee overflow. */
    char fill = 'X';
    for (sz i = 0; i < KLOG_SIZE + 100; i++)
        klog_write(&fill, 1);

    sleep_ms(time_ms_new(20));

    if (klog_dropped() <= dropped_before)
        return 1;
    return 0;
}
DEFINE_SELFTEST(klog_overflow, test_klog_overflow);

/* Test 3: drain consumes data.  Write a marker, drain it, then verify
 * a second drain does not return the same marker (consumption semantics).
 */
static i32 test_klog_drain(void)
{
    const char *marker = "KLOG_DRAIN_TEST_99";
    sz marker_len = 18;
    klog_write(marker, marker_len);

    /* First drain: should consume the marker. */
    static char out[KLOG_SIZE];
    sz n = klog_drain(out, sizeof(out));
    if (n == 0)
        return 1; /* ring should not be empty after write */

    /* Second drain: marker must not reappear. */
    sz n2 = klog_drain(out, sizeof(out));
    for (sz i = 0; i + marker_len <= n2; i++) {
        bool match = true;
        for (sz j = 0; j < marker_len; j++) {
            if (out[i + j] != marker[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return 1; /* marker survived drain — consumption broken */
    }
    return 0;
}
DEFINE_SELFTEST(klog_drain, test_klog_drain);
