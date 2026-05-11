/* SPDX-License-Identifier: MIT */
/* EDF (Earliest Deadline First) scheduling class.
 *
 * Deadline tasks live on a separate per-CPU run queue checked before the
 * priority bitmap.  Pick-next is O(n) scan for minimum absolute deadline
 * among non-throttled tasks -- acceptable for the target task count (< 64).
 *
 * Budget: each deadline task has a per-period runtime budget.  When exhausted,
 * the task is throttled (TD_STATE_DL_THROTTLED) and a callout replenishes the
 * budget at the next period boundary.
 *
 * Admission control: sum(runtime/period) across all admitted tasks must stay
 * below CONFIG_SCHED_DL_UTIL_THRESHOLD (default 95%).  Uses Q20 fixed-point.
 *
 * Lock ordering: sched_lock -> pcpu_runq_lock[cpu] (same as core.c).
 * Admission control holds sched_lock.  Enqueue/pick hold pcpu_runq_lock.
 */

#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/list.h>
#include <mazu/pcpu.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>

/* Shared with core.c: per-CPU run queue locks and pcpu_array.
 * Declared in core.c; accessed here under the same lock protocol.
 */
extern spinlock_t pcpu_runq_lock[];
extern struct pcpu pcpu_array[];
extern spinlock_t sched_lock;

/* Per-CPU deadline run queues.  Tasks link via sleep_list (same node
 * used for priority run queues -- a DL task is in exactly one queue).
 */
static struct list_head pcpu_dl_runq[MAX_CPUS];
static u32 pcpu_dl_count[MAX_CPUS];

/* Admission control: Q20 fixed-point utilization tracking.
 * Protected by sched_lock (same lock used for global_next_id).
 */
#define DL_UTIL_SHIFT 20
static u64 dl_total_util;
static u32 dl_nr_admitted;

/* Telemetry counters (atomics, no lock needed). */
static u64 dl_stat_misses;
static u64 dl_stat_throttles;
static u64 dl_stat_replenishments;

void sched_dl_init(void)
{
    for (u32 c = 0; c < MAX_CPUS; c++) {
        list_init(&pcpu_dl_runq[c]);
        pcpu_dl_count[c] = 0;
    }
    dl_total_util = 0;
    dl_nr_admitted = 0;
}

void sched_dl_task_init(struct sched_task *task)
{
    task->td_policy = SCHED_POLICY_NORMAL;
    memset(&task->dl, 0, sizeof(task->dl));
    callout_init(&task->dl.dl_replenish_callout);
}

void sched_dl_task_destroy(struct sched_task *task)
{
    sched_dl_clearattr(task);
    callout_cancel_sync(&task->dl.dl_replenish_callout);
}

static u64 ns_to_ticks(u64 ns)
{
    return time_usec_to_ticks(ns / 1000);
}

static void dl_deadline_missed(struct sched_task *task)
{
    u64 now = time_rdtime();
    u64 miss_ticks = now - task->dl.dl_abs_deadline;
    u64 miss_us = time_ticks_to_us(miss_ticks);
    (void) miss_us;

    __atomic_fetch_add(&dl_stat_misses, 1, __ATOMIC_RELAXED);

    KTRACE("event=dl_deadline_miss cpu=%hu tid=%hu miss_us=%lu",
           (u32) get_cpuid(), (u32) task->id, miss_us);
}

/* Replenishment callout: runs in ISR context.
 * Restores budget, advances deadline, and re-enqueues if throttled.
 */
static void dl_replenish_cb(void *arg)
{
    struct sched_task *task = arg;
    assert(task);

    struct sched_dl_entity *dl = &task->dl;
    dl->dl_remaining = dl->dl_runtime;
    dl->dl_abs_deadline += dl->dl_period;

    /* If the deadline is still in the past after advancing by one period,
     * the task missed multiple periods.  Skip forward to avoid a
     * replenishment storm (delay=1 looping).  Drop missed periods by
     * advancing to now + dl_deadline.
     */
    u64 now = time_rdtime();
    if (now > dl->dl_abs_deadline) {
        dl_deadline_missed(task);
        dl->dl_abs_deadline = now + dl->dl_deadline;
    }

    dl->dl_throttled = false;

    __atomic_fetch_add(&dl_stat_replenishments, 1, __ATOMIC_RELAXED);

    /* Re-enqueue only if the task was actually in DL_THROTTLED state.
     * If it was blocked/sleeping for another reason (futex, semaphore),
     * the original wake path handles re-enqueueing; the throttle flag
     * is already cleared above so the task won't be re-throttled on
     * the next enqueue.
     *
     * READY is a third case: the task is already sitting in
     * pcpu_dl_runq[cpu] but sched_dl_pick_next was skipping it because
     * dl_throttled was set.  Now that we cleared the flag the task is
     * pickable, but the owning hart has not been notified -- if it is
     * the per-hart idle thread in wfi (pcpu_runq_bitmap == 0, only DL
     * entries on the runq), it will stay there until something else
     * pokes it.  Force a reschedule on its CPU.
     */
    if (task->state == TD_STATE_DL_THROTTLED) {
        sched_wake_ready(task);
    } else if (task->state == TD_STATE_READY) {
        u32 cpu = task->td_last_cpu;
        if (cpu < MAX_CPUS) {
            struct pcpu *tpc = &pcpu_array[cpu];
            __atomic_store_n(&tpc->need_resched, 1, __ATOMIC_RELEASE);
#if CONFIG_SMP
            if (cpu != get_cpuid())
                ipi_send(cpu, IPI_SCHED);
#endif
        }
    }
}

/* Admission check: can (runtime_ticks, period_ticks) be added without
 * exceeding the utilization threshold?
 * Caller must hold sched_lock.
 */
static bool dl_admission_ok(u64 runtime_ticks, u64 period_ticks)
{
    /* Guard against overflow: runtime_ticks << 20 must fit in u64.
     * Reject if runtime_ticks >= 2^44 (~17.6 trillion ticks).
     */
    if (runtime_ticks >= (1ULL << (64 - DL_UTIL_SHIFT)))
        return false;
    u64 new_util = (runtime_ticks << DL_UTIL_SHIFT) / period_ticks;
    u64 threshold =
        ((u64) CONFIG_SCHED_DL_UTIL_THRESHOLD << DL_UTIL_SHIFT) / 100;
    return (dl_total_util + new_util) <= threshold;
}

int sched_dl_setattr(struct sched_task *task,
                     u64 runtime_ns,
                     u64 deadline_ns,
                     u64 period_ns)
{
    if (runtime_ns == 0 || deadline_ns == 0 || period_ns == 0)
        return -(i32) EINVAL;
    if (runtime_ns > deadline_ns || deadline_ns > period_ns)
        return -(i32) EINVAL;

    u64 rt = ns_to_ticks(runtime_ns);
    u64 dl = ns_to_ticks(deadline_ns);
    u64 pd = ns_to_ticks(period_ns);
    if (rt == 0 || dl == 0 || pd == 0)
        return -(i32) EINVAL;

    /* Admission control under sched_lock. */
    u64 flags = spin_lock_irqsave(&sched_lock);

    bool already_dl =
        task->td_policy == SCHED_POLICY_DEADLINE && task->dl.dl_active;
    u64 old_util =
        already_dl ? (task->dl.dl_runtime << DL_UTIL_SHIFT) / task->dl.dl_period
                   : 0;

    if (already_dl)
        dl_total_util -= old_util;

    if (!dl_admission_ok(rt, pd)) {
        if (already_dl)
            dl_total_util += old_util;
        spin_unlock_irqrestore(&sched_lock, flags);
        return -(i32) EBUSY;
    }

    u64 new_util = (rt << DL_UTIL_SHIFT) / pd;
    dl_total_util += new_util;
    if (!already_dl)
        dl_nr_admitted++;

    /* Set DL parameters inside sched_lock to prevent races with
     * concurrent setattr/clearattr on the same task.
     */
    task->dl.dl_runtime = rt;
    task->dl.dl_deadline = dl;
    task->dl.dl_period = pd;
    task->dl.dl_remaining = rt;
    task->dl.dl_abs_deadline = time_rdtime() + dl;
    task->dl.dl_throttled = false;
    task->dl.dl_active = true;
    task->td_policy = SCHED_POLICY_DEADLINE;

    spin_unlock_irqrestore(&sched_lock, flags);

    return 0;
}

void sched_dl_clearattr(struct sched_task *task)
{
    u64 flags = spin_lock_irqsave(&sched_lock);
    if (task->td_policy != SCHED_POLICY_DEADLINE || !task->dl.dl_active) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    u64 old_util = (task->dl.dl_runtime << DL_UTIL_SHIFT) / task->dl.dl_period;
    dl_total_util -= old_util;
    dl_nr_admitted--;
    task->dl.dl_active = false;
    task->td_policy = SCHED_POLICY_NORMAL;
    spin_unlock_irqrestore(&sched_lock, flags);

    callout_cancel_sync(&task->dl.dl_replenish_callout);
}

void sched_dl_enqueue(struct sched_task *task, u32 cpu)
{
    assert(task->td_policy == SCHED_POLICY_DEADLINE);
    assert(task->dl.dl_active);

    /* Skip if already linked (double-enqueue guard, same as core.c). */
    if (task->sleep_list.next != &task->sleep_list)
        return;

    sched_set_task_state(task, TD_STATE_READY);
    list_add_tail(&pcpu_dl_runq[cpu], &task->sleep_list);
    pcpu_dl_count[cpu]++;

    KTRACE("event=dl_enqueue cpu=%hu tid=%hu abs_dl=%lu", (u32) cpu,
           (u32) task->id, task->dl.dl_abs_deadline);

    /* DL tasks always preempt non-DL tasks.  If current is also DL,
     * preempt only if this task has an earlier deadline.
     */
    struct pcpu *pc = &pcpu_array[cpu];
    struct sched_task *cur = pc->curthread;
    if (cur) {
        bool preempt = cur->td_policy != SCHED_POLICY_DEADLINE ||
                       task->dl.dl_abs_deadline < cur->dl.dl_abs_deadline;
        if (preempt)
            __atomic_store_n(&pc->need_resched, 1, __ATOMIC_RELEASE);
    }

    /* Telemetry. */
    pc->nr_enqueue++;
    pc->nr_sched_ops++;
    if (pc->nr_sched_ops > pc->max_sched_ops)
        pc->max_sched_ops = pc->nr_sched_ops;

#if CONFIG_SMP
    if (cpu != get_cpuid()) {
        get_pcpu()->nr_remote_wakeups++;
        ipi_send(cpu, IPI_SCHED);
    }
#endif
}

struct sched_task *sched_dl_pick_next(u32 cpu)
{
    if (pcpu_dl_count[cpu] == 0)
        return NULL;

    struct sched_task *best = NULL;
    struct sched_task *task;

    list_for_each_entry_safe (&pcpu_dl_runq[cpu], task, struct sched_task,
                              sleep_list) {
        if (task->dl.dl_throttled)
            continue;
        if (!best || task->dl.dl_abs_deadline < best->dl.dl_abs_deadline)
            best = task;
    }

    if (!best)
        return NULL;

    /* Dequeue the winner. */
    list_del_init(&best->sleep_list);
    pcpu_dl_count[cpu]--;

    /* Dequeue telemetry. */
    struct pcpu *dpc = &pcpu_array[cpu];
    dpc->nr_dequeue++;
    if (dpc->nr_sched_ops > 0)
        dpc->nr_sched_ops--;

    KTRACE("event=dl_pick cpu=%hu tid=%hu abs_dl=%lu", (u32) cpu,
           (u32) best->id, best->dl.dl_abs_deadline);

    return best;
}

void sched_dl_charge(struct sched_task *task, u64 delta_ticks)
{
    struct sched_dl_entity *dl = &task->dl;

    if (!dl->dl_active || dl->dl_throttled)
        return;

    /* Detect deadline miss while running: if the absolute deadline
     * has passed, report it now (once per job).
     */
    u64 now = time_rdtime();
    if (now > dl->dl_abs_deadline)
        dl_deadline_missed(task);

    if (delta_ticks >= dl->dl_remaining) {
        dl->dl_remaining = 0;
        dl->dl_throttled = true;

        __atomic_fetch_add(&dl_stat_throttles, 1, __ATOMIC_RELAXED);

        /* Only overwrite state to DL_THROTTLED if the task is still
         * RUNNING.  If the task transitioned to SLEEPING/BLOCKED/
         * SEM_WAIT before budget exhaustion was checked, preserve
         * the original block reason.  The replenishment callout
         * checks for DL_THROTTLED specifically before re-enqueueing.
         */
        if (task->state == TD_STATE_RUNNING)
            sched_set_task_state(task, TD_STATE_DL_THROTTLED);

        KTRACE("event=dl_throttle cpu=%hu tid=%hu", (u32) get_cpuid(),
               (u32) task->id);

        /* Arm replenishment callout for the remaining period time. */
        u64 period_start = dl->dl_abs_deadline - dl->dl_deadline;
        u64 next_replenish = period_start + dl->dl_period;
        u64 delay = (next_replenish > now) ? (next_replenish - now) : 1;
        callout_set_ticks(&dl->dl_replenish_callout, delay, dl_replenish_cb,
                          task);
    } else {
        dl->dl_remaining -= delta_ticks;
    }
}

void sched_dl_get_stats(struct sched_dl_stats *out)
{
    out->nr_admitted = __atomic_load_n(&dl_nr_admitted, __ATOMIC_RELAXED);
    out->nr_deadline_misses =
        __atomic_load_n(&dl_stat_misses, __ATOMIC_RELAXED);
    out->nr_throttles = __atomic_load_n(&dl_stat_throttles, __ATOMIC_RELAXED);
    out->nr_replenishments =
        __atomic_load_n(&dl_stat_replenishments, __ATOMIC_RELAXED);
}

#include __INC_TEST(deadline)
