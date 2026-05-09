/* SPDX-License-Identifier: MIT */
/* DAG-based init system - topological sort engine.
 *
 * Uses Kahn's algorithm over initgraph_task descriptors collected by the
 * linker into the .initgraph section.  All working storage is in BSS.
 * no heap allocation is needed (this runs before the allocator in some
 * configurations).
 *
 * The graph is bipartite: tasks and stages.  A task with
 * INIT_ENTAILS(stage) has an edge task -> stage; a task with
 * INIT_REQUIRES(stage) has an edge stage -> task.  This is flattened into
 * a task-to-task dependency: for every pair (provider, consumer) where
 * provider entails stage S and consumer requires stage S, consumer
 * depends on provider.  Kahn's algorithm then yields a valid execution
 * order.
 */

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/print.h>

extern const struct initgraph_task __initgraph_start[];
extern const struct initgraph_task __initgraph_end[];

/* BSS working arrays sized for INITGRAPH_MAX_TASKS. */
static u16 in_degree[INITGRAPH_MAX_TASKS];
static u16 queue[INITGRAPH_MAX_TASKS];

/* For each stage, track which tasks entail it and how many.
 * A stage is "satisfied" when all providers have executed.
 */
struct stage_providers {
    u8 task_idx[INITGRAPH_MAX_TASKS]; /* indices of provider tasks */
    u8 count;                         /* number of providers */
};

static struct stage_providers providers[INITGRAPH_MAX_STAGES];

void initgraph_run(u32 lifecycle_flag)
{
    ALWAYS_ASSERT(lifecycle_flag != 0);

    const struct initgraph_task *base = __initgraph_start;
    sz n_total = __initgraph_end - __initgraph_start;

    ALWAYS_ASSERT(n_total >= 0);
    ALWAYS_ASSERT(n_total <= INITGRAPH_MAX_TASKS);

    if (n_total == 0)
        return;

    u16 n = (u16) n_total;

    /* Filter: collect indices of tasks matching this lifecycle flag. */
    u8 active_idx[INITGRAPH_MAX_TASKS]; /* maps filtered position -> global
                                           index
 */
    u16 n_active = 0;

    for (u16 i = 0; i < n; i++) {
        if ((base[i].flags & lifecycle_flag) != 0)
            active_idx[n_active++] = (u8) i;
    }

    if (n_active == 0)
        return;

    /* Build the stage -> provider mapping for active tasks only. */
    for (u16 s = 0; s < INITGRAPH_MAX_STAGES; s++)
        providers[s].count = 0;

    for (u16 ai = 0; ai < n_active; ai++) {
        u8 gi = active_idx[ai];
        for (u16 e = 0; e < 2; e++) {
            u8 stage = base[gi].entails[e];
            if (stage == INITGRAPH_STAGE_NONE)
                continue;
            ALWAYS_ASSERT(stage < INITGRAPH_MAX_STAGES);
            struct stage_providers *sp = &providers[stage];
            ALWAYS_ASSERT(sp->count < INITGRAPH_MAX_TASKS);
            sp->task_idx[sp->count++] = (u8) ai;
        }
    }

    /* Compute in-degree for each active task.
     * For each active task that requires stage S, add an edge from every
     * active provider of S to this task.
     */
    for (u16 ai = 0; ai < n_active; ai++)
        in_degree[ai] = 0;

    for (u16 ai = 0; ai < n_active; ai++) {
        u8 gi = active_idx[ai];
        for (u16 r = 0; r < 2; r++) {
            u8 stage = base[gi].
                           requires[
                               r];
            if (stage == INITGRAPH_STAGE_NONE)
                continue;
            ALWAYS_ASSERT(stage < INITGRAPH_MAX_STAGES);
            in_degree[ai] += providers[stage].count;
        }
    }

    /* Kahn's algorithm: seed the queue with zero-in-degree tasks. */
    u16 head = 0, tail = 0;
    for (u16 ai = 0; ai < n_active; ai++) {
        if (in_degree[ai] == 0)
            queue[tail++] = ai;
    }

    u16 executed = 0;
    while (head < tail) {
        u16 ai = queue[head++];
        u8 gi = active_idx[ai];
        const struct initgraph_task *t = &base[gi];

#if __DEBUG__ > 0
        pr_debug(STR("initgraph: run \"%s\" flags=0x%hx\n"), t->name,
                 (u16) lifecycle_flag);
#endif

        t->fn(lifecycle_flag);
        executed++;

        /* For each stage this task entails, decrement in-degree of all
         * tasks that require that stage.
         */
        for (u16 e = 0; e < 2; e++) {
            u8 stage = t->entails[e];
            if (stage == INITGRAPH_STAGE_NONE)
                continue;
            for (u16 ci = 0; ci < n_active; ci++) {
                u8 cgi = active_idx[ci];
                for (u16 r = 0; r < 2; r++) {
                    if (base[cgi].requires[r] == stage) {
                        ALWAYS_ASSERT(in_degree[ci] > 0);
                        in_degree[ci]--;
                        if (in_degree[ci] == 0)
                            queue[tail++] = ci;
                    }
                }
            }
        }
    }

    /* Cycle detection: if any active task still has in_degree > 0, the
     * graph contains a cycle.  Report the first offender and halt.
     */
    if (executed != n_active) {
        for (u16 ai = 0; ai < n_active; ai++) {
            if (in_degree[ai] > 0) {
                u8 gi = active_idx[ai];
                pr_err(STR("initgraph: cycle detected at \"%s\"\n"),
                       base[gi].name);
                break;
            }
        }
        ALWAYS_ASSERT(executed == n_active);
    }

    pr_info(STR("initgraph: %hu tasks executed (flags=0x%lx)\n"), executed,
            (u64) lifecycle_flag);
}

#include __INC_TEST(initgraph)
