/* SPDX-License-Identifier: MIT */
#ifndef MAZU_SELFTEST_H
#define MAZU_SELFTEST_H

#include <mazu/base.h>

#if CONFIG_SEMIHOSTING

struct selftest {
    const char *name;
    i32 (*test_fn)(void); /* 0 = pass, nonzero = fail */
};

#define DEFINE_SELFTEST(sname, fn)       \
    static __used __section(".selftest") \
        __aligned(8) struct selftest __selftest_##sname = {#sname, fn}

i32 run_selftests(void);
void selftest_check_and_run(void);

#else /* !CONFIG_SEMIHOSTING */

/* No-op: test functions are compiled but never collected into .selftest.
 * The __unused attribute suppresses -Wunused-function warnings.
 */
#define DEFINE_SELFTEST(sname, fn) \
    static __unused i32 (*__selftest_ref_##sname)(void) = fn

static inline void selftest_check_and_run(void) {}

#endif /* CONFIG_SEMIHOSTING */
#endif /* MAZU_SELFTEST_H */
