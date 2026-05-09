/* SPDX-License-Identifier: MIT */
/* Per-task resource tracking - auto-destroyed on task exit.
 *
 * Named kres (kernel resource), inspired by Linux devres.  Each kres
 * is linked into the owning task's kres_list.  On task termination,
 * kres_destroy_all iterates the list and calls each destructor.
 *
 * Ownership contract (no locking needed):
 *   kres_register / kres_unregister: called only by the owning task.
 *   kres_destroy_all: called from deferred-free after task is unreachable.
 */

#ifndef MAZU_KRES_H
#define MAZU_KRES_H

#include <mazu/list.h>

struct sched_task; /* forward declaration */

#define KRES_MAGIC 0x6B726573U /* ASCII "kres" */

struct kres {
    struct list_head node;
    u32 magic;
    struct sched_task *owner;
    void (*destroy)(struct kres *);
};

void kres_init(struct kres *kr);
void kres_register(struct kres *kr, void (*destroy)(struct kres *));
void kres_unregister(struct kres *kr);
void kres_destroy_all(struct sched_task *td);

#endif /* MAZU_KRES_H */
