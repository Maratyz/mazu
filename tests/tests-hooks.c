/* SPDX-License-Identifier: MIT */
#include <mazu/initgraph.h>
#include <mazu/selftest.h>

static volatile i32 hook_test_resume_core_count;
static volatile i32 hook_test_resume_late_count;
static volatile i32 hook_test_secondary_only_count;
static volatile i32 hook_test_order[4];
static volatile i32 hook_test_order_idx;

/* Test stages for ordering: "core" runs before "late".
 * Reuses INITGRAPH_STAGE_SCHED as the barrier between them - the
 * "core" task entails SCHED, the "late" task requires SCHED.  This
 * mirrors the old INIT_LEVEL_CORE < INIT_LEVEL_LATE ordering.
 */

static void hook_test_resume_core(u32 lifecycle_flag __unused)
{
    hook_test_resume_core_count++;
    hook_test_order[hook_test_order_idx++] = 11;
}
INIT_TASK("test_resume_core",
          hook_test_resume_core,
          INIT_REQUIRES_NONE,
          INIT_ENTAILS(INITGRAPH_STAGE_SCHED),
          INIT_FLAG_CPU_RESUME);

static void hook_test_resume_late(u32 lifecycle_flag __unused)
{
    hook_test_resume_late_count++;
    hook_test_order[hook_test_order_idx++] = 22;
}
INIT_TASK("test_resume_late",
          hook_test_resume_late,
          INIT_REQUIRES(INITGRAPH_STAGE_SCHED),
          INIT_ENTAILS_NONE,
          INIT_FLAG_CPU_RESUME);

static void hook_test_secondary_only(u32 lifecycle_flag __unused)
{
    hook_test_secondary_only_count++;
}
INIT_TASK("test_secondary_only",
          hook_test_secondary_only,
          INIT_REQUIRES_NONE,
          INIT_ENTAILS_NONE,
          INIT_FLAG_SECONDARY);

static i32 test_init_hooks_resume_dispatch(void)
{
    hook_test_resume_core_count = 0;
    hook_test_resume_late_count = 0;
    hook_test_secondary_only_count = 0;
    hook_test_order_idx = 0;
    hook_test_order[0] = hook_test_order[1] = hook_test_order[2] =
        hook_test_order[3] = -1;

    initgraph_run(INIT_FLAG_CPU_RESUME);

    if (hook_test_resume_core_count != 1)
        return 1;
    if (hook_test_resume_late_count != 1)
        return 1;
    if (hook_test_secondary_only_count != 0)
        return 1;

    /* Verify ordering: core (11) must appear before late (22) in the
     * execution log.  Other CPU_RESUME tasks from tests-initgraph.c
     * may interleave, so scan for subsequence rather than exact match.
     */
    i32 saw_core = -1, saw_late = -1;
    for (i32 i = 0; i < hook_test_order_idx; i++) {
        if (hook_test_order[i] == 11 && saw_core < 0)
            saw_core = i;
        if (hook_test_order[i] == 22 && saw_late < 0)
            saw_late = i;
    }
    if (saw_core < 0 || saw_late < 0)
        return 1;
    if (saw_core >= saw_late)
        return 1;

    return 0;
}
DEFINE_SELFTEST(init_hooks_resume_dispatch, test_init_hooks_resume_dispatch);
