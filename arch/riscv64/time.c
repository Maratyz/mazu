/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit time subsystem.
 *
 * Uses the 'time' CSR (read from machine-mode mtime via the unprivileged
 * interface) as the monotonic clock source. The timebase frequency is resolved
 * from the FDT at init time; falls back to 10 MHz (QEMU virt).
 *
 * Timer interrupts (scause = SCAUSE_STIMER) are driven by the callout engine:
 * sbi_set_timer() is programmed to the earliest callout deadline and disarmed
 * entirely (U64_MAX) when no callouts are pending.
 *
 * The SBI timer is inherently per-hart: each hart programs its own mtimecmp via
 * SBI, so no SMP changes are needed for the tick handler.
 *
 * Hot-path tick conversions (ticks->ms, ticks->us) use precomputed fixed-point
 * multipliers (FreqFraction pattern from managarm) so they compile to a single
 * multiply+shift with no division.
 */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/callout.h>
#include <mazu/pcpu.h>
#include <mazu/time.h>
#include <sbi.h>

#define TIMEBASE_FREQ_DEFAULT 10000000ULL

/* Resolved at init from FDT or fallback. */
static u64 timebase_freq;

static bool global_time_initialized;
static u64 global_time_base;

static void time_init_hart(void);

/* Fixed-point multiplier for division-free tick conversion (Q12).
 *
 * To convert ticks to target units (ms or us):
 *   result = ticks * numerator / denominator
 *
 * Precomputed values:
 *   frac = (numerator << shift) / denominator
 *
 * Then at runtime:
 *   result = (u128)(ticks * frac) >> shift
 *
 * On RISC-V, 64-bit division is 30+ cycles. The fixed-point multiply is 2
 * instructions (mul + mulhu for the 128-bit intermediate, then shift). The
 * division runs once at boot in freq_frac_init().
 */

struct freq_frac {
    u64 mul;   /* fixed-point multiplier */
    int shift; /* right-shift amount */
};

/* Precomputed: ticks * 1000 / freq (ticks -> ms) */
static struct freq_frac frac_to_ms;

/* Precomputed: ticks * 1000000 / freq (ticks -> us) */
static struct freq_frac frac_to_us;

/* Precomputed: ms * freq / 1000 (ms -> ticks) */
static struct freq_frac frac_from_ms;

/* Precomputed: us * freq / 1000000 (us -> ticks) */
static struct freq_frac frac_from_us;

static int log2_floor(u64 x)
{
    int r = 0;
    while (x > 1) {
        x >>= 1;
        r++;
    }
    return r;
}

/* Compute a fixed-point multiplier: result = (numerator << s) / denominator
 * where s is chosen to maximize precision while keeping mul within 64 bits.
 */
static struct freq_frac freq_frac_init(u64 numerator, u64 denominator)
{
    assert(denominator > 0);
    assert(numerator > 0);

    struct freq_frac ff;
    /* Choose shift so that (numerator << shift) fits in 64 bits. */
    ff.shift = 63 - log2_floor(numerator);
    if (ff.shift < 0)
        ff.shift = 0;
    ff.mul = ((numerator << ff.shift) + denominator / 2) / denominator;
    return ff;
}

/* Apply the fixed-point conversion: (ticks * ff.mul) >> ff.shift.
 * Uses 128-bit intermediate; saturates to U64_MAX on overflow.
 */
static inline u64 freq_frac_apply(struct freq_frac ff, u64 ticks)
{
    __uint128_t product = (__uint128_t) ff.mul * ticks;
    product >>= ff.shift;
    return (product >> 64) ? U64_MAX : (u64) product;
}

static inline u64 rdtime(void)
{
    u64 t;
    __asm__ volatile("csrr %0, time" : "=r"(t));
    return t;
}

static inline u64 time_pcpu_merged_deadline(const struct pcpu *pc)
{
    u64 combined = pc->timer_deadline;
    if (pc->preempt_deadline < combined)
        combined = pc->preempt_deadline;
    if (pc->watchdog_deadline < combined)
        combined = pc->watchdog_deadline;
    return combined;
}

void time_init(void)
{
    assert(!global_time_initialized);

    /* Resolve timebase from FDT, fall back to QEMU-virt default 10 MHz. */
    timebase_freq = board_info.timebase_freq ? board_info.timebase_freq
                                             : TIMEBASE_FREQ_DEFAULT;

    /* Precompute fixed-point multipliers (one-time division at boot). */
    frac_to_ms = freq_frac_init(1000ULL, timebase_freq);
    frac_to_us = freq_frac_init(1000000ULL, timebase_freq);
    frac_from_ms = freq_frac_init(timebase_freq, 1000ULL);
    frac_from_us = freq_frac_init(timebase_freq, 1000000ULL);

    global_time_base = rdtime();

    time_init_hart();

    /* Mark initialized BEFORE enabling global interrupts: the timer could fire
     * immediately after sstatus.SIE is set, and time_current_ms() asserts
     * global_time_initialized.
     */
    global_time_initialized = true;

    enable_interrupts();
}

/* Per-hart timer initialization: callout subsystem, merged deadline state, SIE
 * enable, and disarm. Called from time_init (BSP) and time_init_secondary
 * (secondary harts).
 */
static void time_init_hart(void)
{
    callout_subsys_init();

    /* All deadline sources disarmed. */
    struct pcpu *pc = get_pcpu();
    pc->timer_deadline = U64_MAX;
    pc->preempt_deadline = U64_MAX;
    pc->watchdog_deadline = U64_MAX;
    pc->current_deadline = U64_MAX;
    pc->clockevent_mode = CLOCKEVENT_SHUTDOWN;
    pc->nr_timer_writes = 0;
    pc->nr_timer_skips = 0;

    /* Heartbeat: initialized here, stamped by scheduler progress points.
     * No cross-hart readers yet (hart not online), but use atomics for
     * consistency with the runtime access pattern.
     */
    __atomic_store_n(&pc->heartbeat_stamp, rdtime(), __ATOMIC_RELAXED);
    __atomic_store_n(&pc->heartbeat_stale, false, __ATOMIC_RELAXED);

    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    sbi_set_timer(U64_MAX);
}

/* Initialize timer on a secondary hart. Called during secondary boot when
 * secondaries enter the scheduler.
 */
void time_init_secondary(void)
{
    time_init_hart();
}

struct time_ms time_current_ms(void)
{
    assert(global_time_initialized);
    u64 elapsed = rdtime() - global_time_base;
    return time_ms_new(freq_frac_apply(frac_to_ms, elapsed));
}

u64 time_rdtime(void)
{
    return rdtime();
}

u64 time_ticks_to_us(u64 ticks)
{
    return freq_frac_apply(frac_to_us, ticks);
}

u64 time_ticks_to_ms(u64 ticks)
{
    return freq_frac_apply(frac_to_ms, ticks);
}

u64 time_ms_to_ticks(u64 ms)
{
    return freq_frac_apply(frac_from_ms, ms);
}

u64 time_usec_to_ticks(u64 usec)
{
    return freq_frac_apply(frac_from_us, usec);
}

u64 time_get_timebase_freq(void)
{
    return timebase_freq;
}

/* Merged deadline management.
 *
 * Three independent deadline sources per hart: timer_deadline (callout engine),
 * preempt_deadline (quantum expiry), and watchdog_deadline (per-hart
 * heartbeat). Computes min across all three and only programs sbi_set_timer()
 * when the result differs from the current HW deadline.  This eliminates
 * redundant SBI calls when a deadline source is updated without changing the
 * effective next-wakeup time.
 *
 * Also updates the clockevent mode: CLOCKEVENT_ONESHOT when any deadline is
 * armed (< U64_MAX), CLOCKEVENT_SHUTDOWN when all sources are disarmed.
 */
void update_pcpu_deadline(void)
{
    struct pcpu *pc = get_pcpu();
    DEBUG_ASSERT(pc == &pcpu_array[get_cpuid()]);
    u64 combined = time_pcpu_merged_deadline(pc);

    pc->clockevent_mode =
        (combined == U64_MAX) ? CLOCKEVENT_SHUTDOWN : CLOCKEVENT_ONESHOT;

    if (combined != pc->current_deadline) {
        pc->current_deadline = combined;
        sbi_set_timer(combined);
        pc->nr_timer_writes++;
    } else {
        pc->nr_timer_skips++;
    }

    DEBUG_ASSERT(pc->current_deadline == time_pcpu_merged_deadline(pc));
    DEBUG_ASSERT((pc->clockevent_mode == CLOCKEVENT_SHUTDOWN) ==
                 (pc->current_deadline == U64_MAX));
}

/* Called by trap.c when scause == SCAUSE_STIMER. */
void time_handle_timer_interrupt(void)
{
    struct pcpu *pc = get_pcpu();
    /* current_deadline may be U64_MAX if a cancel/reprogram raced with
     * an already-pending timer IRQ.  The handler safely ignores this:
     * no deadline source will match, and update_pcpu_deadline re-arms
     * or disarms as needed.
     */

    /* The deadline that just fired is the current_deadline. Clear the source
     * that matched so it doesn't re-fire immediately.
     */
    u64 now = rdtime();
    if (pc->timer_deadline <= now)
        pc->timer_deadline = U64_MAX;
    if (pc->preempt_deadline <= now)
        pc->preempt_deadline = U64_MAX;
    if (pc->watchdog_deadline <= now)
        pc->watchdog_deadline = U64_MAX;

    /* Do NOT stamp heartbeat here.  A hart wedged in kernel code with
     * interrupts enabled still receives timer IRQs, so stamping here
     * would mask the stall.  Heartbeat advances only at real forward-
     * progress points: sched_schedule, sched_yield_trap,
     * sched_note_activity.
     */

    callout_process();
}
