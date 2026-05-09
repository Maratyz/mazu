/* SPDX-License-Identifier: MIT */
/* Selftest: KTRACE structured event log format verification.
 *
 * Emits known KTRACE events and verifies they appear in the klog ring
 * with the expected prefix and key=value format.  This test validates
 * the emission path; semantic validation (state machine invariants) is
 * done by scripts/check_eventv1.py on the host side.
 */

#include <mazu/eventlog.h>
#include <mazu/klog.h>
#include <mazu/string.h>

#ifdef CONFIG_EVENTLOG

/* Use the existing str_find_substring from string.h instead of
 * rolling a custom substring search.
 */
static bool eventlog_contains(char *buf, sz len, struct str needle)
{
    return !str_find_substring(str_new(buf, len), needle).is_none;
}

static i32 test_eventlog_format(void)
{
    /* Drain any existing klog data for a clean start.  Bound the loop
     * to avoid livelock if other harts keep emitting KTRACE records.
     */
    char drain_buf[2048];
    for (int drain_pass = 0; drain_pass < 16 && klog_has_data(); drain_pass++)
        klog_drain(drain_buf, sizeof(drain_buf));

    /* Emit a known KTRACE event.  fmt + klog_write is synchronous, so the
     * event is in the klog ring immediately after this call.
     */
    KTRACE("event=test_event tid=%hu cpu=%hu val=%lu", (u32) 42, (u32) 0,
           (u64) 12345);

    /* Peek at klog.  Use a large buffer to capture any scheduler events
     * that other harts emitted between the drain and peek.
     */
    char buf[2048];
    sz got = klog_peek(buf, sizeof(buf));
    if (got == 0)
        return -1; /* no output captured */

    /* Verify the KTRACE prefix (with rdtime timestamp) and key=value pairs.
     * The full prefix is "KTRACE t=<rdtime> event=...", so check for
     * "KTRACE t=" and the event/key fields independently.
     */
    if (!eventlog_contains(buf, got, STR("KTRACE t=")))
        return -2;
    if (!eventlog_contains(buf, got, STR("event=test_event")))
        return -3;
    if (!eventlog_contains(buf, got, STR("tid=42")))
        return -4;
    if (!eventlog_contains(buf, got, STR("cpu=0")))
        return -5;
    if (!eventlog_contains(buf, got, STR("val=12345")))
        return -6;

    return 0;
}

static i32 test_eventlog_task_state_names(void)
{
    /* Verify all KTRACE task state name mappings return non-empty strings. */
    struct str s;

    s = ktrace_td_state(TD_STATE_READY);
    if (s.len == 0)
        return -1;
    s = ktrace_td_state(TD_STATE_RUNNING);
    if (s.len == 0)
        return -2;
    s = ktrace_td_state(TD_STATE_SLEEPING);
    if (s.len == 0)
        return -3;
    s = ktrace_td_state(TD_STATE_SEM_WAIT);
    if (s.len == 0)
        return -4;
    s = ktrace_td_state(TD_STATE_YIELDING);
    if (s.len == 0)
        return -5;
    s = ktrace_td_state(TD_STATE_TERMINATING);
    if (s.len == 0)
        return -6;
    s = ktrace_td_state(TD_STATE_BLOCKED);
    if (s.len == 0)
        return -7;

    return 0;
}

DEFINE_SELFTEST(eventlog_format, test_eventlog_format);
DEFINE_SELFTEST(eventlog_task_state_names, test_eventlog_task_state_names);

#else /* !CONFIG_EVENTLOG */

/* When CONFIG_EVENTLOG is disabled, verify the macro compiles to nothing. */
static i32 test_eventlog_disabled(void)
{
    /* This should be a no-op with zero overhead when disabled. */
    KTRACE("event=should_not_appear tid=%hu", (u32) 0);
    return 0;
}

DEFINE_SELFTEST(eventlog_disabled, test_eventlog_disabled);

#endif /* CONFIG_EVENTLOG */
