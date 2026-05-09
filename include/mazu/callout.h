/* SPDX-License-Identifier: MIT */
/* Tickless callout subsystem.
 *
 * One-shot timer callbacks driven by absolute rdtime deadlines.  Per-CPU
 * sorted lists with per-CPU spinlocks.  Hardware timer is programmed via
 * sbi_set_timer() and disarmed entirely when no work is pending (tickless
 * idle).
 *
 * Callbacks execute in ISR context - they must be short O(1) operations
 * (state update + enqueue, no allocation, no blocking).
 */

#ifndef MAZU_CALLOUT_H
#define MAZU_CALLOUT_H

#include <mazu/base.h>
#include <mazu/list.h>

/* Callout flags. */
#define CALLOUT_FLAG_ARMED BIT(0)   /* callout is in a per-CPU list */
#define CALLOUT_FLAG_RUNNING BIT(1) /* callback is executing (cancel_sync) */

/* Callback signature: runs in ISR context, must be O(1) and non-blocking. */
typedef void (*callout_fn_t)(void *arg);

struct callout {
    u64 deadline;          /* absolute rdtime tick */
    callout_fn_t func;     /* callback function */
    void *arg;             /* callback argument */
    volatile u32 flags;    /* CALLOUT_FLAG_ARMED | CALLOUT_FLAG_RUNNING */
    u32 cpu;               /* owning hart (for cross-hart cancel) */
    struct list_head node; /* per-CPU sorted list linkage */
};

/* Zero-initialize a callout.  Does not arm it. */
void callout_init(struct callout *c);

/* Arm a callout to fire after 'ticks' rdtime ticks from now.
 * If already armed, re-arms (unlinks old, inserts new deadline).
 * Safe to call from ISR or thread context.
 */
void callout_set_ticks(struct callout *c,
                       u64 ticks,
                       callout_fn_t func,
                       void *arg);

/* Convenience: arm a callout to fire after 'usec' microseconds. */
void callout_set_usec(struct callout *c,
                      u64 usec,
                      callout_fn_t func,
                      void *arg);

/* Cancel an armed callout.  No-op if not armed.
 * Reprograms the hardware timer if the canceled callout was at the head.
 * On cross-hart head cancellation, sends IPI_TIMER so the owning hart
 * recomputes its merged deadline and reprograms the hardware timer immediately.
 * Safe for cross-hart cancel (retry loop on CPU mismatch).
 */
void callout_cancel(struct callout *c);

/* Cancel + spin-wait for any in-flight callback to complete.
 * Use before freeing memory that embeds a callout struct.
 */
void callout_cancel_sync(struct callout *c);

/* Return true if the callout is armed (will fire in the future). */
static inline bool callout_pending(const struct callout *c)
{
    return (c->flags & CALLOUT_FLAG_ARMED) != 0;
}

/* ISR entry point: process all expired callouts on the current hart,
 * then reprogram the hardware timer for the next deadline (or disarm).
 */
void callout_process(void);

/* Initialize per-hart callout list.  Called from time_init (BSP) and
 * time_init_secondary (secondary harts).
 */
void callout_subsys_init(void);

/* Return true if the current hart's callout list is empty. */
bool callout_list_empty(void);

/* Callout telemetry: aggregated across all harts. */
struct callout_stats {
    u64 dispatched;   /* total callbacks dispatched */
    u64 missed;       /* callbacks fired after deadline (> 100us late) */
    u64 max_late_us;  /* worst-case lateness in microseconds */
    u64 timer_writes; /* sbi_set_timer() calls (merged deadline) */
    u64 timer_skips;  /* HW writes skipped (deadline unchanged) */
    /* Jitter histogram: lateness distribution across all dispatched callouts.
     * Bins: [0] 0-10us, [1] 11-100us, [2] 101us-1ms, [3] 1-10ms,
     *        [4] 10-100ms, [5] >100ms.
     */
    u64 late_hist[6];
};

/* Recompute the local hart's timer_deadline from its callout list and
 * reprogram the hardware timer.  Called from the IPI handler after a
 * remote cross-hart head cancellation.
 */
void callout_reprogram_local(void);

/* Snapshot callout telemetry counters (summed across all CPUs). */
void callout_get_stats(struct callout_stats *out);

#endif /* MAZU_CALLOUT_H */
