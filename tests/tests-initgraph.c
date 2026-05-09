/* SPDX-License-Identifier: MIT */
/* Self-tests for the DAG-based initgraph system. */
#include <mazu/initgraph.h>
#include <mazu/selftest.h>

/* Test stages and tasks. */

/* Test stages: private to this file, offset past production stages. */
#define TEST_STAGE_A 3
#define TEST_STAGE_B 4
#define TEST_STAGE_C 5

static_assert(TEST_STAGE_C < INITGRAPH_MAX_STAGES,
              "test stages exceed INITGRAPH_MAX_STAGES");

/* Execution log for verifying order and counts. */
static volatile i32 ig_test_order[8];
static volatile i32 ig_test_idx;

/* Reset the test log. */
static void ig_test_reset(void)
{
    ig_test_idx = 0;
    for (i32 i = 0; i < (i32) countof(ig_test_order); i++)
        ig_test_order[i] = -1;
}

/* Test init functions. */

static void ig_task_a(u32 lifecycle_flag __unused)
{
    if (ig_test_idx < (i32) countof(ig_test_order))
        ig_test_order[ig_test_idx++] = 1;
}
/* Task A: no requires, entails TEST_STAGE_A.  Must run first. */
INIT_TASK("test_a",
          ig_task_a,
          INIT_REQUIRES_NONE,
          INIT_ENTAILS(TEST_STAGE_A),
          INIT_FLAG_CPU_RESUME);

static void ig_task_b(u32 lifecycle_flag __unused)
{
    if (ig_test_idx < (i32) countof(ig_test_order))
        ig_test_order[ig_test_idx++] = 2;
}
/* Task B: requires TEST_STAGE_A, entails TEST_STAGE_B.  Runs after A. */
INIT_TASK("test_b",
          ig_task_b,
          INIT_REQUIRES(TEST_STAGE_A),
          INIT_ENTAILS(TEST_STAGE_B),
          INIT_FLAG_CPU_RESUME);

static void ig_task_c(u32 lifecycle_flag __unused)
{
    if (ig_test_idx < (i32) countof(ig_test_order))
        ig_test_order[ig_test_idx++] = 3;
}
/* Task C: requires TEST_STAGE_B, entails TEST_STAGE_C.  Runs after B. */
INIT_TASK("test_c",
          ig_task_c,
          INIT_REQUIRES(TEST_STAGE_B),
          INIT_ENTAILS(TEST_STAGE_C),
          INIT_FLAG_CPU_RESUME);

/* Task D: SECONDARY-only, should NOT run for CPU_RESUME. */
static void ig_task_d(u32 lifecycle_flag __unused)
{
    if (ig_test_idx < (i32) countof(ig_test_order))
        ig_test_order[ig_test_idx++] = 99;
}
INIT_TASK("test_d",
          ig_task_d,
          INIT_REQUIRES_NONE,
          INIT_ENTAILS_NONE,
          INIT_FLAG_SECONDARY);

/* Selftest: topological order. */

static i32 test_initgraph_order(void)
{
    ig_test_reset();

    /* Run CPU_RESUME lifecycle: picks up tasks A, B, C (and test hooks
     * from tests-hooks.c) but not D (SECONDARY) or production PRIMARY
     * tasks.  Re-running CPU_RESUME is safe because those tasks are idempotent.
     */
    initgraph_run(INIT_FLAG_CPU_RESUME);

    /* The tasks wrote 1, 2, 3 to ig_test_order.  Other CPU_RESUME tasks
     * (from tests-hooks.c) write to separate globals.  Verify the
     * subsequence 1 -> 2 -> 3 appears in order.
     */
    if (ig_test_idx < 3)
        return 1;

    i32 prev = 0;
    i32 found = 0;
    for (i32 i = 0; i < ig_test_idx; i++) {
        i32 val = ig_test_order[i];
        if (val == prev + 1) {
            prev = val;
            found++;
            if (found == 3)
                break;
        }
    }
    if (found != 3)
        return 1;

    /* Task D (SECONDARY-only) must NOT have run. */
    for (i32 i = 0; i < ig_test_idx; i++) {
        if (ig_test_order[i] == 99)
            return 1;
    }

    return 0;
}
DEFINE_SELFTEST(initgraph_order, test_initgraph_order);

/* Selftest: lifecycle filtering. */

/* Test that SECONDARY-only tasks do not fire under CPU_RESUME.
 * Cannot safely re-run INIT_FLAG_PRIMARY because production tasks
 * (sched_init, tcp_init) are not idempotent.  Instead, verify the
 * SECONDARY counter stays at zero across CPU_RESUME invocations.
 */
static volatile i32 ig_secondary_counter;

static void ig_secondary_probe(u32 lifecycle_flag __unused)
{
    ig_secondary_counter++;
}
INIT_TASK("test_sec_probe",
          ig_secondary_probe,
          INIT_REQUIRES_NONE,
          INIT_ENTAILS_NONE,
          INIT_FLAG_SECONDARY);

static i32 test_initgraph_lifecycle_filter(void)
{
    i32 old = ig_secondary_counter;

    /* CPU_RESUME must not trigger SECONDARY tasks. */
    initgraph_run(INIT_FLAG_CPU_RESUME);

    if (ig_secondary_counter != old)
        return 1;

    return 0;
}
DEFINE_SELFTEST(initgraph_lifecycle_filter, test_initgraph_lifecycle_filter);
