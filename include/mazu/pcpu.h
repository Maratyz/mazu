/* SPDX-License-Identifier: MIT */
/* Per-hart scheduler, timer, and trap state.
 *
 * On SMP builds, the gp register points to the current hart's struct pcpu.
 * That makes trap entry, timer reprogramming, and scheduler bookkeeping all
 * operate on strictly local state without extra indirection.
 *
 * On UP builds, hart 0 still uses the same layout so the invariants stay
 * identical.
 */

#ifndef MAZU_PCPU_H
#define MAZU_PCPU_H

#include <mazu/base.h>

/* Forward declaration; full definition in sched.h. */
struct sched_task;

/* Clockevent mode: per-hart timer operating mode.
 * SHUTDOWN: timer fully disarmed (U64_MAX), no interrupts.
 * ONESHOT:  fire once at next_expiry, disarm after delivery.
 * The kernel is tickless; periodic mode is intentionally absent.
 */
enum clockevent_mode {
    CLOCKEVENT_SHUTDOWN = 0,
    CLOCKEVENT_ONESHOT = 1,
};

#if !CONFIG_SMP
#define MAX_CPUS 1
#elif CONFIG_CPU_MAX
#define MAX_CPUS CONFIG_CPU_MAX
#else
#define MAX_CPUS 8
#endif

/* Per-hart interrupt stack size: 16 KiB. */
#define INTR_STACK_SIZE 0x4000

struct pcpu {
    struct sched_task *curthread; /* offset 0: current running task */
    u64 *intr_stack_top;          /* offset 8: per-hart interrupt stack top */
    u32 hartid;                   /* physical hart ID */
    u32 cpuid;                    /* logical CPU index 0..N-1 */
    u32 trap_nest;                /* nested trap depth on this hart */
    u32 irq_nest;                 /* interrupt (async) nesting depth */
    u32 critnest;                 /* critical_enter nesting depth */
    u64 saved_sie;             /* sstatus.SIE saved on first critical_enter */
    volatile u32 pending_ipis; /* IPI bitmask, set atomically by sender */
    bool online;               /* true once secondary_init_c completes */
    bool idle;                 /* true when parked in wfi */
    u32 need_resched;          /* set by ISR/IPI; unconditionally drained by
                                  trap_dispatch at schedule_decision (preempts only
                                  if curthread is still TD_STATE_RUNNING) */

    /* Per-hart interrupt counters - unconditional, useful for diagnostics
     * and tickless validation (proving zero timer IRQs during idle).
     */
    u64 nr_timer; /* supervisor timer interrupts (STIMER) */
    u64 nr_exti;  /* supervisor external interrupts (PLIC) */
    u64 nr_ssi;   /* supervisor software interrupts (IPI/yield) */

    /* Per-hart scheduler activity counters for telemetry.
     * Updated under pcpu_runq_lock from enqueue/dequeue paths.
     * With per-CPU run queues, these track the local queue depth
     * on each hart.
     */
    u64 nr_enqueue;       /* total tasks enqueued by this hart */
    u64 nr_dequeue;       /* total tasks dequeued (picked) by this hart */
    u32 nr_sched_ops;     /* enqueue - dequeue on this hart (activity gauge) */
    u32 max_sched_ops;    /* high-water mark of nr_sched_ops since boot */
    u64 total_wait_ticks; /* cumulative ticks tasks spent in run queue */

    /* Per-hart context-switch counters for telemetry.
     * nr_ctxsw: total task transitions (old != next) on this hart.
     * ctxsw_cycles_total: cumulative rdtime ticks in the scheduler path
     *   (sched_schedule entry to return, includes lock + pick + accounting).
     * ctxsw_cycles_max: worst-case single scheduler-path duration in ticks.
     */
    u64 nr_ctxsw;
    u64 ctxsw_cycles_total;
    u64 ctxsw_cycles_max;
    u64 nr_migrations;     /* tasks migrated TO this hart from another */
    u64 nr_remote_wakeups; /* cross-hart enqueues (IPI-initiated) */

    /* Per-hart merged deadline state.
     * The timer hardware always tracks the minimum of callout, quantum, and
     * watchdog deadlines. update_pcpu_deadline() is the single merge point.
     */
    u64 timer_deadline;    /* earliest callout deadline on this hart */
    u64 preempt_deadline;  /* quantum expiry deadline (U64_MAX = none) */
    u64 watchdog_deadline; /* per-hart watchdog expiry (U64_MAX = none) */
    u64 current_deadline;  /* min(timer, preempt, watchdog) */
    u64 nr_timer_writes;   /* sbi_set_timer() calls (telemetry) */
    u64 nr_timer_skips;    /* deadline unchanged, HW write skipped */
    u64 hart_load;         /* sum of task load_avg on this hart (Q16) */

    /* Clockevent mode for this hart.  Tracks whether the HW timer is
     * shutdown (no deadline sources active) or one-shot (at least one
     * deadline source armed).  Updated by update_pcpu_deadline().
     */
    u8 clockevent_mode; /* enum clockevent_mode */

    /* Per-hart watchdog heartbeat state.
     * Monotonic stamp updated only at scheduler/trap/timer progress points so a
     * stale value means the hart stopped making RT forward progress.
     */
    u64 heartbeat_stamp;   /* rdtime at last progress point */
    u64 heartbeat_timeout; /* configurable timeout in ticks */
    bool heartbeat_stale;  /* true when detected as stalled */

    /* Per-hart uaccess fault-recovery state.
     * When a page fault occurs during copy_{to,from}_user, trap_dispatch
     * redirects execution to uaccess_recover_sepc and reports -EFAULT.
     */
    bool uaccess_active;
    bool uaccess_faulted;
    u64 uaccess_recover_sepc;
    u64 uaccess_read_page;
    u64 uaccess_read_gen;
    u64 uaccess_write_page;
    u64 uaccess_write_gen;

#ifdef CONFIG_SCHED_DEADLINE
    /* RT-aware IPI filtering: set when running a SCHED_DEADLINE
     * or SCHED_PRIO_REALTIME task.  sched_balance_load() skips sending
     * IPI_SCHED to this hart unless urgency criteria are met.
     */
    bool rt_active;
    u64 rt_deadline; /* absolute deadline of current RT task (or U64_MAX) */
#endif

    /* Per-hart idle state tracking (item 22).
     * ACTIVE:  running a non-idle task.
     * SHALLOW: in wfi, timer armed at next callout/quantum deadline.
     * DEEP:    in wfi, periodic tick suppressed, timer armed at
     *          next-callout deadline only (entered when next deadline
     *          exceeds the deep-idle threshold).
     */
    u8 idle_state; /* enum pcpu_idle_state */

    /* Lock ordering enforcement (lockdep, 6d).
     * Bitmask of currently held lock levels on this hart.
     * Bit N set = a lock at level N is held.  Only used when __DEBUG__ > 0.
     */
    u16 held_locks;
} __aligned(64); /* cache-line aligned to prevent false sharing */

/* Idle state enum for item 22 deep idle tracking. */
enum pcpu_idle_state {
    PCPU_IDLE_ACTIVE = 0,  /* running non-idle task */
    PCPU_IDLE_SHALLOW = 1, /* wfi, timer armed normally */
    PCPU_IDLE_DEEP = 2,    /* wfi, periodic tick suppressed */
};

/* Verify assembly-critical offsets at compile time. */
static_assert(offsetof(struct pcpu, curthread) == 0,
              "pcpu.curthread must be "
              "at offset 0 for asm");
static_assert(offsetof(struct pcpu, intr_stack_top) == 8,
              "pcpu.intr_stack_top "
              "must be at offset 8 "
              "for asm");

extern struct pcpu pcpu_array[MAX_CPUS];
extern char intr_stacks[MAX_CPUS][INTR_STACK_SIZE] __aligned(16);

/* Atomic counter of online harts. */
extern volatile u32 nr_cpus_online;

/* Read the per-CPU pointer from the gp register.
 * With -mno-relax, gp is free for kernel use.
 */
static inline struct pcpu *get_pcpu(void)
{
#if CONFIG_SMP
    struct pcpu *pc;
    __asm__ volatile("mv %0, gp" : "=r"(pc));
    return pc;
#else
    return &pcpu_array[0];
#endif
}

static inline u32 get_hartid(void)
{
    return get_pcpu()->hartid;
}

static inline u32 get_cpuid(void)
{
    return get_pcpu()->cpuid;
}

static inline bool in_trap_context(void)
{
    return get_pcpu()->trap_nest != 0;
}

/* True when running inside an asynchronous interrupt handler (timer, external,
 * SSI).  Synchronous exceptions (ecall/syscall, page fault) do NOT count
 * because syscall handlers may legitimately block or allocate.
 */
static inline bool in_interrupt_context(void)
{
    return get_pcpu()->irq_nest != 0;
}

/* Recompute the merged deadline from timer_deadline and preempt_deadline.
 * Only calls sbi_set_timer() when the combined minimum changed, avoiding
 * spurious HW timer writes.  Call after modifying either deadline source.
 *
 * Must be called on the local hart only (reads/writes local pcpu fields).
 */
void update_pcpu_deadline(void);

/* Snapshot of per-hart interrupt counters for stats reporting. */
struct pcpu_irq_stats {
    u64 nr_timer;
    u64 nr_exti;
    u64 nr_ssi;
};

static inline struct pcpu_irq_stats pcpu_irq_stats_get(u32 cpu)
{
    struct pcpu *pc = &pcpu_array[cpu];
    return (struct pcpu_irq_stats) {
        .nr_timer = pc->nr_timer,
        .nr_exti = pc->nr_exti,
        .nr_ssi = pc->nr_ssi,
    };
}

/* Snapshot of per-hart scheduler run-queue counters for stats reporting. */
struct pcpu_sched_stats {
    u64 nr_enqueue;
    u64 nr_dequeue;
    u32 nr_sched_ops;
    u32 max_sched_ops;
    u64 total_wait_ticks;
};

static inline struct pcpu_sched_stats pcpu_sched_stats_get(u32 cpu)
{
    struct pcpu *pc = &pcpu_array[cpu];
    return (struct pcpu_sched_stats) {
        .nr_enqueue = pc->nr_enqueue,
        .nr_dequeue = pc->nr_dequeue,
        .nr_sched_ops = pc->nr_sched_ops,
        .max_sched_ops = pc->max_sched_ops,
        .total_wait_ticks = pc->total_wait_ticks,
    };
}

/* Snapshot of per-hart context-switch counters for stats reporting. */
struct pcpu_ctxsw_stats {
    u64 nr_ctxsw;
    u64 ctxsw_cycles_total;
    u64 ctxsw_cycles_max;
    u64 nr_migrations;
    u64 nr_remote_wakeups;
};

static inline struct pcpu_ctxsw_stats pcpu_ctxsw_stats_get(u32 cpu)
{
    struct pcpu *pc = &pcpu_array[cpu];
    return (struct pcpu_ctxsw_stats) {
        .nr_ctxsw = pc->nr_ctxsw,
        .ctxsw_cycles_total = pc->ctxsw_cycles_total,
        .ctxsw_cycles_max = pc->ctxsw_cycles_max,
        .nr_migrations = pc->nr_migrations,
        .nr_remote_wakeups = pc->nr_remote_wakeups,
    };
}

/* Deep idle threshold: minimum ticks until next callout to enter DEEP idle.
 * Default 10ms at 10 MHz timebase = 100,000 ticks.  Actual threshold is
 * computed at runtime from the discovered timebase frequency.
 */
#define DEEP_IDLE_THRESHOLD_MS 10

/* Transition the current hart's idle state.  Called from the scheduler
 * idle loop and from the timer/IPI wake path.
 */
void pcpu_idle_enter(enum pcpu_idle_state state);
void pcpu_idle_exit(void);

#endif /* MAZU_PCPU_H */
