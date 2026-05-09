/* SPDX-License-Identifier: MIT */
/* DAG-based init system.
 *
 * Replaces the level-based init hook framework with a dependency graph.
 * Tasks declare which stages they require (must be satisfied before running)
 * and which stages they entail (satisfied after running).  A topological sort
 * (Kahn's algorithm) determines execution order.
 *
 * All data lives in BSS or linker sections - no heap allocation needed.
 */

#ifndef MAZU_INITGRAPH_H
#define MAZU_INITGRAPH_H

#include <mazu/base.h>

/* Maximum graph dimensions.  Sized for current kernel; bump if needed. */
#define INITGRAPH_MAX_TASKS 32
#define INITGRAPH_MAX_STAGES 8

/* Sentinel: "no stage dependency in this slot". */
#define INITGRAPH_STAGE_NONE 0xFF

/* Stage identifiers.  Each stage is a synchronization barrier - it carries
 * no code, only ordering semantics.  A stage is "satisfied" when every task
 * that entails it has finished executing.
 */
enum initgraph_stage {
    INITGRAPH_STAGE_SCHED = 0,  /* scheduler is ready */
    INITGRAPH_STAGE_NET = 1,    /* TCP/network subsystem is ready */
    INITGRAPH_STAGE_SUBSYS = 2, /* all subsystems ready */
    /* add new stages here */
    INITGRAPH_STAGE_COUNT /* must be last */
};

static_assert(INITGRAPH_STAGE_COUNT <= INITGRAPH_MAX_STAGES,
              "too many "
              "initgraph "
              "stages");

typedef void (*initgraph_fn_t)(u32 lifecycle_flag);

/* An initgraph task descriptor.  Placed in the .initgraph linker section
 * by the INIT_TASK macro.
 */
struct initgraph_task {
    const char *name;  /* human-readable, for debug/cycle reports */
    initgraph_fn_t fn; /* init function */
    u8
        requires[
            2];    /* stage IDs this task needs (STAGE_NONE = unused) */
    u8 entails[2]; /* stage IDs this task satisfies */
    u16 flags;     /* lifecycle flags (INIT_FLAG_PRIMARY, etc.) */
};

/* Place a task descriptor in the .initgraph linker section.
 *
 * Usage:
 *   INIT_TASK("sched", sched_init_hook,
 *             INIT_REQUIRES_NONE, INIT_ENTAILS(INITGRAPH_STAGE_SCHED),
 *             INIT_FLAG_PRIMARY);
 */
#define INIT_TASK(name_, fn_, req_, ent_, flags_)                \
    static const __used __section(".initgraph")                  \
        __aligned(8) struct initgraph_task __initgraph_##fn_ = { \
            .name = (name_),                                     \
            .fn = (fn_),                                         \
            .requires = {req_},                                  \
            .entails = {ent_},                                   \
            .flags = (u16) (flags_),                             \
    }

/* Dependency helpers: expand to two comma-separated u8 values for the
 * requires[] or entails[] array initialiser.
 */
#define INIT_REQUIRES(s) (u8)(s), INITGRAPH_STAGE_NONE
#define INIT_REQUIRES2(a, b) (u8)(a), (u8) (b)
#define INIT_REQUIRES_NONE INITGRAPH_STAGE_NONE, INITGRAPH_STAGE_NONE
#define INIT_ENTAILS(s) (u8)(s), INITGRAPH_STAGE_NONE
#define INIT_ENTAILS2(a, b) (u8)(a), (u8) (b)
#define INIT_ENTAILS_NONE INITGRAPH_STAGE_NONE, INITGRAPH_STAGE_NONE

/* Run all initgraph tasks whose flags match lifecycle_flag.
 * Tasks are executed in topological order; a cycle triggers ALWAYS_ASSERT.
 */
void initgraph_run(u32 lifecycle_flag);

#endif /* MAZU_INITGRAPH_H */
