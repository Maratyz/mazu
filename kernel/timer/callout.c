/* SPDX-License-Identifier: MIT */
/* Tickless callout engine.
 *
 * Per-CPU sorted linked list of one-shot timers.  Hardware timer is
 * programmed to the earliest deadline via sbi_set_timer() and disarmed
 * entirely (U64_MAX) when no callouts are pending.
 *
 * Callbacks run in ISR context with the per-CPU lock dropped to prevent
 * deadlock if the callback re-arms a callout.
 *
 * On SMP, cross-hart cancel uses a retry loop: read c->cpu, lock that
 * CPU's list, verify c->cpu hasn't changed (callback may have migrated
 * the callout), retry if stale.
 */

#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/ipi.h>
#include <mazu/pcpu.h>
#include <mazu/spinlock.h>
#include <mazu/string.h>
#include <mazu/time.h>

#include "../lockdep.h"

/* Per-hart sorted callout list and its protecting lock. */
static struct list_head callout_list[MAX_CPUS];
static spinlock_t callout_lock[MAX_CPUS];

/* Per-hart telemetry counters (updated in ISR context). */
static u64 callout_dispatched[MAX_CPUS];
static u64 callout_missed[MAX_CPUS];
static u64 callout_max_late_ticks[MAX_CPUS];
static u64 callout_late_hist[MAX_CPUS][6]; /* jitter histogram per-CPU */

/* Internal helpers

 * Update the timer_deadline in the local pcpu and recompute the merged
 * deadline.  Caller holds the per-CPU callout lock.  If the list is
 * empty, sets timer_deadline to U64_MAX (disarmed).
 */
static void callout_reprogram_locked(u32 cpu)
{
    DEBUG_ASSERT(cpu == get_cpuid());

    struct pcpu *pc = get_pcpu();
    if (list_empty(&callout_list[cpu])) {
        pc->timer_deadline = U64_MAX;
    } else {
        struct callout *head =
            list_entry(callout_list[cpu].next, struct callout, node);
        pc->timer_deadline = head->deadline;
    }
    update_pcpu_deadline();
}

/* Recompute the local hart's merged deadline from its callout list.
 * Called from the IPI handler after a remote cross-hart head cancellation.
 */
void callout_reprogram_local(void)
{
    u32 cpu = get_cpuid();
    u64 flags = intr_disable();
    lockdep_acquire(LOCK_LEVEL_CALLOUT);
    spin_lock(&callout_lock[cpu]);
    callout_reprogram_locked(cpu);
    spin_unlock(&callout_lock[cpu]);
    lockdep_release(LOCK_LEVEL_CALLOUT);
    intr_restore(flags);
}

/* Insert 'c' into the sorted list for 'cpu'.  Caller holds the lock. */
static void callout_insert_locked(struct callout *c, u32 cpu)
{
    struct list_head *pos = &callout_list[cpu];
    struct callout *cur;

    list_for_each_entry_safe (&callout_list[cpu], cur, struct callout, node) {
        if (cur->deadline > c->deadline) {
            pos = &cur->node;
            break;
        }
    }
    /* Insert before 'pos' (i.e. at tail of elements <= c->deadline). */
    list_add_tail(pos, &c->node);
}

/* Public API */

void callout_init(struct callout *c)
{
    c->deadline = 0;
    c->func = NULL;
    c->arg = NULL;
    c->flags = 0;
    c->cpu = 0;
    list_init(&c->node);
}

void callout_set_ticks(struct callout *c,
                       u64 ticks,
                       callout_fn_t func,
                       void *arg)
{
    assert(c);
    assert(func);

    /* Determine target CPU: if already armed, keep on same CPU;
     * otherwise use the current hart.  Retry loop mirrors
     * callout_cancel for cross-hart safety: if c->cpu changes
     * between the read and lock acquisition, retry.
     */
    u32 cpu;
    u64 flags;
    struct list_head *old_head;

    for (;;) {
        cpu = (c->flags & CALLOUT_FLAG_ARMED) ? c->cpu : get_cpuid();
        assert(cpu < MAX_CPUS);

        flags = intr_disable();
        lockdep_acquire(LOCK_LEVEL_CALLOUT);
        spin_lock(&callout_lock[cpu]);
        old_head = callout_list[cpu].next;

        /* If armed, verify cpu hasn't migrated. */
        if ((c->flags & CALLOUT_FLAG_ARMED) && c->cpu != cpu) {
            spin_unlock(&callout_lock[cpu]);
            lockdep_release(LOCK_LEVEL_CALLOUT);
            intr_restore(flags);
            continue;
        }

        /* Safe to unlink from current position. */
        if (c->flags & CALLOUT_FLAG_ARMED)
            list_del_init(&c->node);
        break;
    }

    /* Clamp deadline to prevent wrap-around: if rdtime() + ticks would
     * overflow u64, set deadline to U64_MAX (effectively infinite - the
     * callout will never fire until canceled or the system reboots).
     * This handles the case where sleep_ms(TIME_MS_MAX) saturates the
     * tick conversion to U64_MAX.
     */
    u64 now = time_rdtime();
    c->deadline = (ticks > U64_MAX - now) ? U64_MAX : now + ticks;
    c->func = func;
    c->arg = arg;
    c->cpu = cpu;
    c->flags |= CALLOUT_FLAG_ARMED;

    callout_insert_locked(c, cpu);

    /* Reprogram only when the earliest deadline (queue head) changed.
     * This covers both "new earliest inserted" and "old head moved later".
     */
    struct list_head *new_head = callout_list[cpu].next;
    if (new_head != old_head || old_head == &c->node) {
        if (cpu == get_cpuid()) {
            callout_reprogram_locked(cpu);
        } else {
            /* Cross-hart head change: send IPI so the target hart can
             * re-evaluate scheduling/timers on trap exit.
             */
            ipi_send(cpu, IPI_SCHED);
        }
    }

    spin_unlock(&callout_lock[cpu]);
    lockdep_release(LOCK_LEVEL_CALLOUT);
    intr_restore(flags);
}

void callout_set_usec(struct callout *c, u64 usec, callout_fn_t func, void *arg)
{
    callout_set_ticks(c, time_usec_to_ticks(usec), func, arg);
}

void callout_cancel(struct callout *c)
{
    assert(c);

    /* Retry loop for cross-hart safety: if the callback re-armed on a
     * different CPU between the read of c->cpu and lock acquisition,
     * the function retries with the new CPU.
     */
    for (;;) {
        if (!(__atomic_load_n(&c->flags, __ATOMIC_RELAXED) &
              CALLOUT_FLAG_ARMED))
            return; /* already fired or canceled */

        u32 cpu = c->cpu;
        u64 flags = intr_disable();
        lockdep_acquire(LOCK_LEVEL_CALLOUT);
        spin_lock(&callout_lock[cpu]);

        if (!(c->flags & CALLOUT_FLAG_ARMED)) {
            spin_unlock(&callout_lock[cpu]);
            lockdep_release(LOCK_LEVEL_CALLOUT);
            intr_restore(flags);
            return;
        }

        if (c->cpu != cpu) {
            /* Migrated while the lock was being acquired; retry. */
            spin_unlock(&callout_lock[cpu]);
            lockdep_release(LOCK_LEVEL_CALLOUT);
            intr_restore(flags);
            continue;
        }

        bool was_head = (callout_list[cpu].next == &c->node);
        list_del_init(&c->node);
        c->flags &= ~(u32) CALLOUT_FLAG_ARMED;

        bool need_remote_ipi = false;

        if (was_head) {
            if (cpu == get_cpuid()) {
                callout_reprogram_locked(cpu);
            } else {
                /* Remote head cancellation must prompt the owning hart to
                 * recompute its merged deadline immediately.  Defer the IPI
                 * until after we release the lock to minimize hold time —
                 * sbi_send_ipi() can take hundreds of cycles.
                 */
                need_remote_ipi = true;
            }
        }

        spin_unlock(&callout_lock[cpu]);
        lockdep_release(LOCK_LEVEL_CALLOUT);
        intr_restore(flags);

        if (need_remote_ipi)
            ipi_send(cpu, IPI_TIMER);

        return;
    }
}

void callout_cancel_sync(struct callout *c)
{
    assert(c);

    for (;;) {
        callout_cancel(c);

        /* Spin-wait for in-flight callback to complete.
         * Pairs with __ATOMIC_RELEASE in callout_process.
         */
        if (!(__atomic_load_n(&c->flags, __ATOMIC_ACQUIRE) &
              CALLOUT_FLAG_RUNNING))
            break;

        while (__atomic_load_n(&c->flags, __ATOMIC_ACQUIRE) &
               CALLOUT_FLAG_RUNNING)
            ;
    }
}

/* Batch-collect pattern: collect up to CALLOUT_BATCH expired callouts
 * under the lock, then invoke all callbacks outside the lock.  Reduces
 * lock acquire/release from N to ceil(N/CALLOUT_BATCH) for N expired
 * callouts.  The outer loop drains all expired callouts regardless of
 * how many expire simultaneously - no callback ever executes under lock.
 */
#define CALLOUT_BATCH 8

void callout_process(void)
{
    u32 cpu = get_cpuid();

    /* Cache callout metadata under lock so it is safe to measure lateness
     * after releasing the lock - concurrent callout_set_ticks() on another
     * hart can overwrite c->deadline once ARMED is cleared.
     */
    struct {
        struct callout *c;
        u64 deadline;
        callout_fn_t func;
        void *arg;
    } batch[CALLOUT_BATCH];

    for (;;) {
        u64 now = time_rdtime();
        int n = 0;

        u64 flags = intr_disable();
        lockdep_acquire(LOCK_LEVEL_CALLOUT);
        spin_lock(&callout_lock[cpu]);

        while (!list_empty(&callout_list[cpu]) && n < CALLOUT_BATCH) {
            struct callout *c =
                list_entry(callout_list[cpu].next, struct callout, node);
            if (c->deadline > now)
                break;

            list_del_init(&c->node);
            batch[n].c = c;
            batch[n].deadline = c->deadline;
            batch[n].func = c->func;
            batch[n].arg = c->arg;
            c->flags &= ~(u32) CALLOUT_FLAG_ARMED;
            c->flags |= CALLOUT_FLAG_RUNNING;
            DEBUG_ASSERT(c->cpu == cpu);
            n++;
        }

        /* If nothing was collected, reprogram and exit - single reprogram
         * point when no more work remains.
         */
        if (n == 0) {
            callout_reprogram_locked(cpu);
            spin_unlock(&callout_lock[cpu]);
            lockdep_release(LOCK_LEVEL_CALLOUT);
            intr_restore(flags);
            return;
        }

        spin_unlock(&callout_lock[cpu]);
        lockdep_release(LOCK_LEVEL_CALLOUT);
        intr_restore(flags);

        for (int i = 0; i < n; i++) {
            /* Measure lateness: how far past deadline this callback fired.
             * Callbacks more than 100us late are counted as missed.
             * Histogram bins: 0-10us, 11-100us, 101us-1ms, 1-10ms,
             * 10-100ms, >100ms (same bins as sched wakeup latency).
             */
            u64 late_ticks = now - batch[i].deadline;
            if (late_ticks > callout_max_late_ticks[cpu])
                callout_max_late_ticks[cpu] = late_ticks;
            u64 late_us = time_ticks_to_us(late_ticks);
            if (late_us > 100)
                callout_missed[cpu]++;

            callout_late_hist[cpu][latency_us_to_bin(late_us)]++;

            batch[i].func(batch[i].arg);
            callout_dispatched[cpu]++;

            /* Atomic release-clear: cancel_sync spins on RUNNING
             * and concurrent set_ticks may set ARMED. A plain
             * RMW would race with those flag updates.
             */
            __atomic_fetch_and(&batch[i].c->flags, ~(u32) CALLOUT_FLAG_RUNNING,
                               __ATOMIC_RELEASE);
        }

        /* Loop back to check for more expired callouts.  Callbacks may
         * have re-armed themselves, and additional callouts may have
         * expired during execution.  Re-sample now on each iteration.
         */
    }
}

void callout_subsys_init(void)
{
    u32 cpu = get_cpuid();
    list_init(&callout_list[cpu]);
    callout_lock[cpu] = (spinlock_t) SPINLOCK_INITIALIZER;
}

bool callout_list_empty(void)
{
    u32 cpu = get_cpuid();
    return list_empty(&callout_list[cpu]);
}

void callout_get_stats(struct callout_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (u32 i = 0; i < MAX_CPUS; i++) {
        out->dispatched += callout_dispatched[i];
        out->missed += callout_missed[i];
        u64 late_us = time_ticks_to_us(callout_max_late_ticks[i]);
        if (late_us > out->max_late_us)
            out->max_late_us = late_us;
        out->timer_writes += pcpu_array[i].nr_timer_writes;
        out->timer_skips += pcpu_array[i].nr_timer_skips;
        for (u32 b = 0; b < countof(out->late_hist); b++)
            out->late_hist[b] += callout_late_hist[i][b];
    }
}

/* Self-tests */

#include __INC_TEST(callout)
