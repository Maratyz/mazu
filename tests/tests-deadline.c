/* SPDX-License-Identifier: MIT */
#include <mazu/sched.h>
#include <mazu/selftest.h>
#include <mazu/time.h>

/* Test: admission control rejects over-utilization. */
static i32 test_dl_admission_reject(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    /* 100% utilization exceeds the 95% default threshold. */
    int rc = sched_dl_setattr(cur, 100000000ULL, 100000000ULL, 100000000ULL);
    if (rc != -(i32) EBUSY) {
        sched_dl_clearattr(cur);
        return 1;
    }

    /* 50% utilization should succeed. */
    rc = sched_dl_setattr(cur, 50000000ULL, 100000000ULL, 100000000ULL);
    if (rc != 0) {
        sched_dl_clearattr(cur);
        return 1;
    }

    sched_dl_clearattr(cur);
    return 0;
}
DEFINE_SELFTEST(dl_admission_reject, test_dl_admission_reject);

/* Test: admission accepts valid params and clearattr reverts. */
static i32 test_dl_admission_accept(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    int rc = sched_dl_setattr(cur, 10000000ULL, 50000000ULL, 100000000ULL);
    if (rc != 0)
        return 1;

    i32 ret = 1;
    if (cur->td_policy != SCHED_POLICY_DEADLINE)
        goto out;
    if (!cur->dl.dl_active)
        goto out;
    if (cur->dl.dl_runtime == 0 || cur->dl.dl_period == 0)
        goto out;

    sched_dl_clearattr(cur);
    if (cur->td_policy != SCHED_POLICY_NORMAL)
        return 1;

    return 0;
out:
    sched_dl_clearattr(cur);
    return ret;
}
DEFINE_SELFTEST(dl_admission_accept, test_dl_admission_accept);

/* Test: invalid parameters rejected. */
static i32 test_dl_setattr_invalid(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    /* runtime > deadline */
    int rc = sched_dl_setattr(cur, 200000000ULL, 100000000ULL, 300000000ULL);
    if (rc != -(i32) EINVAL)
        return 1;

    /* deadline > period */
    rc = sched_dl_setattr(cur, 10000000ULL, 200000000ULL, 100000000ULL);
    if (rc != -(i32) EINVAL)
        return 1;

    /* zero runtime */
    rc = sched_dl_setattr(cur, 0, 100000000ULL, 100000000ULL);
    if (rc != -(i32) EINVAL)
        return 1;

    return 0;
}
DEFINE_SELFTEST(dl_setattr_invalid, test_dl_setattr_invalid);

/* Test: DL run-queue path.
 * Set DL attrs on current task, yield through the real DL enqueue/pick
 * path, verify budget was charged.
 */
static i32 test_dl_runqueue_path(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    struct sched_dl_stats before;
    sched_dl_get_stats(&before);

    i32 ret = 1;

    int rc = sched_dl_setattr(cur, 90000000ULL, 100000000ULL, 100000000ULL);
    if (rc != 0)
        return 1;

    if (cur->td_policy != SCHED_POLICY_DEADLINE || !cur->dl.dl_active)
        goto out;

    /* Yield triggers: sched_dl_charge (budget accounting on switch out),
     * sched_dl_enqueue (re-queue into DL run queue),
     * sched_dl_pick_next (pick us back since we're the only DL task).
     */
    sleep_ms(time_ms_new(0));

    if (cur->dl.dl_remaining >= cur->dl.dl_runtime)
        goto out;

    sched_dl_clearattr(cur);

    struct sched_dl_stats after;
    sched_dl_get_stats(&after);
    if (after.nr_admitted != before.nr_admitted)
        return 1;

    return 0;
out:
    sched_dl_clearattr(cur);
    return ret;
}
DEFINE_SELFTEST(dl_runqueue_path, test_dl_runqueue_path);

/* Test: DL budget charge directly exhausts budget and sets throttle flag.
 * Call sched_dl_charge with a delta exceeding dl_remaining to verify
 * the throttle logic without depending on QEMU timer precision.
 */
static i32 test_dl_charge_throttle(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    struct sched_dl_stats before;
    sched_dl_get_stats(&before);
    i32 ret = 1;

    int rc = sched_dl_setattr(cur, 10000000ULL, 50000000ULL, 100000000ULL);
    if (rc != 0)
        return 1;

    u64 saved_remaining = cur->dl.dl_remaining;
    if (saved_remaining == 0)
        goto out;

    /* Charge more than the remaining budget.  This sets dl_throttled
     * and arms the replenishment callout.  The task state is only
     * overwritten to DL_THROTTLED if it is currently RUNNING -- which
     * it is, since this is the current task.
     */
    sched_dl_charge(cur, saved_remaining + 1);

    if (!cur->dl.dl_throttled)
        goto out;
    if (cur->dl.dl_remaining != 0)
        goto out;

    struct sched_dl_stats mid;
    sched_dl_get_stats(&mid);
    if (mid.nr_throttles <= before.nr_throttles)
        goto out;

    /* Revert state to RUNNING so the scheduler doesn't try to handle
     * us as DL_THROTTLED on the next yield.  Wait for the replenishment
     * callout to fire (period is 100ms, 200ms is generous for QEMU).
     */
    sched_set_task_state(cur, TD_STATE_RUNNING);
    sleep_ms(time_ms_new(200));

    if (cur->dl.dl_throttled)
        goto out;
    if (cur->dl.dl_remaining != cur->dl.dl_runtime)
        goto out;

    sched_dl_get_stats(&mid);
    if (mid.nr_replenishments <= before.nr_replenishments)
        goto out;

    ret = 0;
out:
    sched_dl_clearattr(cur);
    return ret;
}
DEFINE_SELFTEST(dl_charge_throttle, test_dl_charge_throttle);

/* Test: DL task preempts NORMAL task. */
static volatile int dl_exec_order[2];
static volatile int dl_exec_idx;

static void dl_normal_cb(void *ctx __unused)
{
    int idx = __atomic_fetch_add(&dl_exec_idx, 1, __ATOMIC_SEQ_CST);
    if (idx < 2)
        dl_exec_order[idx] = 0;
}

static void dl_deadline_cb(void *ctx __unused)
{
    int idx = __atomic_fetch_add(&dl_exec_idx, 1, __ATOMIC_SEQ_CST);
    if (idx < 2)
        dl_exec_order[idx] = 1;
}

static i32 test_dl_preempts_normal(void)
{
    dl_exec_idx = 0;
    dl_exec_order[0] = dl_exec_order[1] = -1;

    disable_interrupts();

    struct result r1 =
        sched_create_task_prio(dl_normal_cb, NULL, SCHED_PRIO_NORMAL);
    if (r1.is_error) {
        enable_interrupts();
        return 1;
    }

    struct result r2 =
        sched_create_task_prio(dl_deadline_cb, NULL, SCHED_PRIO_REALTIME);
    if (r2.is_error) {
        enable_interrupts();
        return 1;
    }

    enable_interrupts();
    sleep_ms(time_ms_new(100));

    if (dl_exec_order[0] != 1 || dl_exec_order[1] != 0)
        return 1;

    return 0;
}
DEFINE_SELFTEST(dl_preempts_normal, test_dl_preempts_normal);

/* Test: clearattr on destroy releases admission utilization. */
static i32 test_dl_clearattr_idempotent(void)
{
    struct sched_task *cur = get_pcpu()->curthread;
    if (!cur)
        return 1;

    /* Double clearattr should not underflow dl_total_util. */
    sched_dl_clearattr(cur);
    sched_dl_clearattr(cur);

    struct sched_dl_stats stats;
    sched_dl_get_stats(&stats);
    if (stats.nr_admitted > 100)
        return 1; /* sanity: underflow would wrap to huge value */

    return 0;
}
DEFINE_SELFTEST(dl_clearattr_idempotent, test_dl_clearattr_idempotent);

/* Test: telemetry stats API. */
static i32 test_dl_stats(void)
{
    struct sched_dl_stats stats;
    sched_dl_get_stats(&stats);
    (void) stats.nr_admitted;
    (void) stats.nr_deadline_misses;
    return 0;
}
DEFINE_SELFTEST(dl_stats, test_dl_stats);
