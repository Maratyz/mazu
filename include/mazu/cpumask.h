/* SPDX-License-Identifier: MIT */
/* CPU mask API for IPI targeting and CPU set management.
 *
 * cpumask_t is a u64 bitmask -- bit N represents logical CPU N.
 * Enforced by static_assert(MAX_CPUS <= 64).
 *
 * Global CPU masks (active, idle, realtime) are maintained atomically
 * by the scheduler and IPI subsystem.  ipi_send_mask() converts the
 * logical CPU mask to a physical hart mask for a single batched SBI
 * ecall.
 */

#ifndef MAZU_CPUMASK_H
#define MAZU_CPUMASK_H

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/pcpu.h>

typedef u64 cpumask_t;

static_assert(MAX_CPUS <= 64, "cpumask_t is u64; MAX_CPUS must be <= 64");

#define CPUMASK_NONE ((cpumask_t) 0)
#define CPUMASK_ALL \
    ((cpumask_t) ((MAX_CPUS == 64) ? ~(u64) 0 : ((u64) 1 << MAX_CPUS) - 1))

/* Return the single-bit mask for cpu, or 0 if cpu is out of range.
 * DEBUG_ASSERT catches logical errors in debug builds; the runtime
 * guard prevents undefined behavior from shifting by >= 64.
 */
static inline cpumask_t cpumask_bit(u32 cpu)
{
    DEBUG_ASSERT(cpu < MAX_CPUS);
    if (cpu >= 64)
        return 0;
    return (cpumask_t) 1 << cpu;
}

static inline void cpumask_set(cpumask_t *m, u32 cpu)
{
    *m |= cpumask_bit(cpu);
}

static inline void cpumask_clear(cpumask_t *m, u32 cpu)
{
    *m &= ~cpumask_bit(cpu);
}

static inline bool cpumask_test(cpumask_t m, u32 cpu)
{
    return (m & cpumask_bit(cpu)) != 0;
}

static inline cpumask_t cpumask_and(cpumask_t a, cpumask_t b)
{
    return a & b;
}

static inline cpumask_t cpumask_or(cpumask_t a, cpumask_t b)
{
    return a | b;
}

static inline cpumask_t cpumask_andnot(cpumask_t a, cpumask_t b)
{
    return a & ~b;
}

/* Return index of lowest set bit, or MAX_CPUS if empty. */
static inline u32 cpumask_first(cpumask_t m)
{
    if (m == 0)
        return MAX_CPUS;
    return (u32) __builtin_ctzll(m);
}

/* Return next set bit after prev, or MAX_CPUS if none. */
static inline u32 cpumask_next(cpumask_t m, u32 prev)
{
    if (prev >= 63)
        return MAX_CPUS;
    m &= ~(((cpumask_t) 1 << (prev + 1)) - 1);
    return cpumask_first(m);
}

static inline u32 cpumask_popcount(cpumask_t m)
{
    return (u32) __builtin_popcountll(m);
}

#define cpumask_for_each(mask, cpu)                     \
    for ((cpu) = cpumask_first(mask); (cpu) < MAX_CPUS; \
         (cpu) = cpumask_next((mask), (cpu)))

/*
 * Global CPU state masks.
 *
 * Updated atomically by the scheduler:
 *   active_cpus   -- set when a hart comes online, cleared on offline.
 *   idle_cpus     -- set when a hart enters idle (wfi), cleared on wakeup.
 *   realtime_cpus -- set when running SCHED_DEADLINE or SCHED_FIFO task.
 */
/* All fields accessed exclusively via __atomic_* builtins. */
struct mp_state {
    cpumask_t active_cpus;
    cpumask_t idle_cpus;
    cpumask_t realtime_cpus;
};

extern struct mp_state mp_state;

static inline void mp_set_cpu_active(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_or(&mp_state.active_cpus, bit, __ATOMIC_RELEASE);
}

static inline void mp_clear_cpu_active(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_and(&mp_state.active_cpus, ~bit, __ATOMIC_RELEASE);
}

static inline void mp_set_cpu_idle(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_or(&mp_state.idle_cpus, bit, __ATOMIC_RELEASE);
}

static inline void mp_clear_cpu_idle(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_and(&mp_state.idle_cpus, ~bit, __ATOMIC_RELEASE);
}

static inline void mp_set_cpu_realtime(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_or(&mp_state.realtime_cpus, bit, __ATOMIC_RELEASE);
}

static inline void mp_clear_cpu_realtime(u32 cpu)
{
    cpumask_t bit = cpumask_bit(cpu);
    if (bit)
        __atomic_fetch_and(&mp_state.realtime_cpus, ~bit, __ATOMIC_RELEASE);
}

static inline cpumask_t mp_get_active_cpus(void)
{
    return __atomic_load_n(&mp_state.active_cpus, __ATOMIC_ACQUIRE);
}

static inline cpumask_t mp_get_idle_cpus(void)
{
    return __atomic_load_n(&mp_state.idle_cpus, __ATOMIC_RELAXED);
}

static inline cpumask_t mp_get_realtime_cpus(void)
{
    return __atomic_load_n(&mp_state.realtime_cpus, __ATOMIC_RELAXED);
}

#endif /* MAZU_CPUMASK_H */
