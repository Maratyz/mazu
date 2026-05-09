/* SPDX-License-Identifier: MIT */
#include <mazu/assert.h>
#include <mazu/selftest.h>

extern u64 __stack_chk_guard;

static i32 selftest_stack_chk_guard_initialized(void)
{
    /* After boot, __stack_chk_guard must be non-zero. */
    assert(__stack_chk_guard != 0);
    return 0;
}
DEFINE_SELFTEST(stack_chk_guard_initialized,
                selftest_stack_chk_guard_initialized);
