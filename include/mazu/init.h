/* SPDX-License-Identifier: MIT */
/* Lifecycle-aware init hook framework.
 *
 * Unlike initcalls (single BSP boot pass), init hooks carry both
 * initialization level and CPU lifecycle flags so the same hook can be
 * selectively run for primary boot, secondary CPU bring-up, or resume paths.
 */

#ifndef MAZU_INIT_H
#define MAZU_INIT_H

#include <mazu/base.h>

typedef void (*init_hook_fn_t)(u32 lifecycle_flag);

enum init_level {
    INIT_LEVEL_EARLY = 0,
    INIT_LEVEL_CORE = 1,
    INIT_LEVEL_SUBSYS = 2,
    INIT_LEVEL_DEVICE = 3,
    INIT_LEVEL_LATE = 4,
    INIT_LEVEL_MAX,
};

#define INIT_FLAG_PRIMARY BIT(0)
#define INIT_FLAG_SECONDARY BIT(1)
#define INIT_FLAG_CPU_RESUME BIT(2)
#define INIT_FLAG_ALL \
    (INIT_FLAG_PRIMARY | INIT_FLAG_SECONDARY | INIT_FLAG_CPU_RESUME)

struct init_hook {
    init_hook_fn_t fn;
    u16 level;
    u16 flags;
};

#define DEFINE_INIT_HOOK(hookfn_, level_, flags_)               \
    static const __used __section(".inithook")                  \
        __aligned(8) struct init_hook __init_hook_##hookfn_ = { \
            .fn = (hookfn_), .level = (u16) (level_), .flags = (u16) (flags_)}

void do_init_hooks(u32 lifecycle_flag);
void do_init_hooks_resume(void);

#endif /* MAZU_INIT_H */
