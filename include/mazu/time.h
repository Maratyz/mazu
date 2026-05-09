/* SPDX-License-Identifier: MIT */
/* Simple RTC-based time. */

#ifndef MAZU_TIME_H
#define MAZU_TIME_H

#include <mazu/base.h>

#define TIME_MS_MAX U64_MAX

struct time_ms {
    u64 ms;
};

static inline struct time_ms time_ms_new(u64 ms)
{
    return (struct time_ms) {.ms = ms};
}

void time_init(void);
void time_init_secondary(void);

struct time_ms time_current_ms(void);

/* Read the raw monotonic tick counter (rdtime CSR). */
u64 time_rdtime(void);

/* Convert a tick count to microseconds using the resolved timebase frequency.
 */
u64 time_ticks_to_us(u64 ticks);

/* Convert a tick count to milliseconds. */
u64 time_ticks_to_ms(u64 ticks);

/* Convert milliseconds to tick count. */
u64 time_ms_to_ticks(u64 ms);

/* Convert microseconds to tick count. */
u64 time_usec_to_ticks(u64 usec);

/* Return the resolved timebase frequency (ticks per second). */
u64 time_get_timebase_freq(void);

/* Map a microsecond latency to a 6-bin histogram index.
 * Bins: [0] 0-10us, [1] 11-100us, [2] 101us-1ms,
 *       [3] 1-10ms, [4] 10-100ms, [5] >100ms.
 * Shared by callout jitter and scheduler wakeup latency histograms.
 */
static inline u32 latency_us_to_bin(u64 us)
{
    if (us <= 10)
        return 0;
    if (us <= 100)
        return 1;
    if (us <= 1000)
        return 2;
    if (us <= 10000)
        return 3;
    if (us <= 100000)
        return 4;
    return 5;
}

#endif /* MAZU_TIME_H */
