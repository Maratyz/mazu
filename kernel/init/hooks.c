/* SPDX-License-Identifier: MIT */
#include <mazu/assert.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/print.h>

extern const struct init_hook __inithook_start[];
extern const struct init_hook __inithook_end[];

/* Legacy init-hook dispatcher.  Retained for any DEFINE_INIT_HOOK users
 * that have not yet been migrated to INIT_TASK.  New code should use
 * INIT_TASK and initgraph_run() exclusively.
 */
void do_init_hooks(u32 lifecycle_flag)
{
    assert(lifecycle_flag != 0);

    for (u32 level = INIT_LEVEL_EARLY; level < INIT_LEVEL_MAX; level++) {
        for (const struct init_hook *h = __inithook_start; h < __inithook_end;
             h++) {
            if (h->level != level)
                continue;
            if ((h->flags & lifecycle_flag) == 0)
                continue;
#if __DEBUG__ > 0
            pr_debug(STR("init_hook: level=%hu flags=0x%hx fn=%lx "
                         "run=0x%lx\n"),
                     (u16) h->level, (u16) h->flags, (u64) (uptr) h->fn,
                     (u64) lifecycle_flag);
#endif
            h->fn(lifecycle_flag);
        }
    }
}

void do_init_hooks_resume(void)
{
    initgraph_run(INIT_FLAG_CPU_RESUME);
}

#include __INC_TEST(hooks)
