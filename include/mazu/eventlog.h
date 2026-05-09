/* SPDX-License-Identifier: MIT */
/* Structured event log for kernel state transitions.
 *
 * Emits machine-readable KTRACE lines; see the event schema notes for
 * scheduler state changes, TCP state transitions, and syscall attribution.
 * Gated on CONFIG_EVENTLOG - zero overhead when disabled.
 */

#ifndef MAZU_EVENTLOG_H
#define MAZU_EVENTLOG_H

#include <mazu/fmt.h>
#include <mazu/klog.h>
#include <mazu/pcpu.h>
#include <mazu/sched.h>
#include <mazu/time.h>

#ifdef CONFIG_EVENTLOG

/* Write to the klog ring buffer only - not directly to UART.
 * The UART has no per-line atomicity on SMP, so concurrent KTRACE
 * from multiple harts would produce garbled serial output that
 * breaks check_eventv1.py replay parsing.  Instead:
 *   - klog ring: atomic under klog_lock, no interleaving
 *   - /api/klog: serves ring contents as JSON for host-side tools
 *   - make check: curls /api/klog and pipes through check_eventv1.py
 *
 * Each event is prefixed with an rdtime timestamp so host-side tools can
 * sort events from different harts into chronological order.
 */
#define KTRACE(fmtstr, ...)                                   \
    do {                                                      \
        char _ktrace_buf[192];                                \
        struct str_buf _ktrace_sb =                           \
            str_buf_new(_ktrace_buf, 0, sizeof(_ktrace_buf)); \
        fmt(&_ktrace_sb, STR("KTRACE t=%lu " fmtstr "\n"),    \
            (u64) time_rdtime(), ##__VA_ARGS__);              \
        klog_write(_ktrace_buf, _ktrace_sb.len);              \
    } while (0)

/* Uppercase task state names for KTRACE (machine-readable). */
static inline struct str ktrace_td_state(enum td_state s)
{
    switch (s) {
    case TD_STATE_READY:
        return STR("READY");
    case TD_STATE_RUNNING:
        return STR("RUNNING");
    case TD_STATE_SLEEPING:
        return STR("SLEEPING");
    case TD_STATE_SEM_WAIT:
        return STR("SEM_WAIT");
    case TD_STATE_YIELDING:
        return STR("YIELDING");
    case TD_STATE_TERMINATING:
        return STR("TERMINATING");
    case TD_STATE_BLOCKED:
        return STR("BLOCKED");
    default:
        return STR("UNKNOWN");
    }
}

/* Centralized task state setter with KTRACE emission.
 * Mirrors tcp_set_state() pattern: single choke-point for all task
 * state transitions ensures complete event coverage.
 * Filters idle-priority tasks to avoid flooding the klog ring.
 * No-op transitions (old == new) are suppressed.
 */
static inline void sched_set_task_state(struct sched_task *td,
                                        enum td_state new_state)
{
    if (td->state != new_state && td->td_prio > SCHED_PRIO_IDLE)
        KTRACE("event=task_state_changed tid=%hu old=%s new=%s cpu=%hu",
               (u32) td->id, ktrace_td_state(td->state),
               ktrace_td_state(new_state), (u32) get_cpuid());
    td->state = new_state;
}

#else /* !CONFIG_EVENTLOG */

#define KTRACE(fmt, ...) ((void) 0)

/* When CONFIG_EVENTLOG is disabled, direct assignment with zero overhead. */
static inline void sched_set_task_state(struct sched_task *td,
                                        enum td_state new_state)
{
    td->state = new_state;
}

#endif /* CONFIG_EVENTLOG */

#endif /* MAZU_EVENTLOG_H */
