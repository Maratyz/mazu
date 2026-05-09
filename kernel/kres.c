/* SPDX-License-Identifier: MIT */
/* Per-task resource cleanup (kres).
 *
 * Each kres is linked into the owning task's kres_list via list_add_tail
 * (FIFO insert).  kres_destroy_all iterates in reverse (LIFO order) so
 * resources are torn down in the opposite order they were registered -
 * matching the expectation that later resources depend on earlier ones.
 */

#include "kres.h"
#include <mazu/assert.h>
#include <mazu/list.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <mazu/sched.h>

void kres_init(struct kres *kr)
{
    assert(kr);
    list_init(&kr->node);
    kr->magic = KRES_MAGIC;
    kr->owner = NULL;
    kr->destroy = NULL;
}

void kres_register(struct kres *kr, void (*destroy)(struct kres *))
{
    assert(kr);
    assert(destroy);
    assert(kr->magic == KRES_MAGIC);
    /* Guard against double-register: node must be self-referential (unlinked).
     */
    assert(kr->node.next == &kr->node);
    assert(kr->owner == NULL);

    struct sched_task *td = get_pcpu()->curthread;
    assert(td);

    kr->owner = td;
    kr->destroy = destroy;
    list_add_tail(&td->kres_list, &kr->node);
}

void kres_unregister(struct kres *kr)
{
    assert(kr);
    assert(kr->magic == KRES_MAGIC);
    list_del_init(&kr->node);
    kr->owner = NULL;
    kr->destroy = NULL;
}

void kres_destroy_all(struct sched_task *td)
{
    assert(td);

    /* Pop from tail (LIFO order) so later resources are destroyed first.
     * This pattern is safe even if a destroy callback unregisters other
     * entries - the tail is always re-read the tail from the live list.
     */
    while (!list_empty(&td->kres_list)) {
        struct kres *kr = list_entry(td->kres_list.prev, struct kres, node);
        if (kr->magic != KRES_MAGIC || kr->owner != td) {
            pr_err(STR("kres: corrupt owner/list for task %hu (kr=%p "
                       "magic=%lx owner=%p)\n"),
                   td->id, kr, (u64) kr->magic, kr->owner);
            list_init(&td->kres_list);
            return;
        }

        void (*destroy)(struct kres *) = kr->destroy;
        list_del_init(&kr->node);
        kr->owner = NULL;
        kr->destroy = NULL;
        if (destroy)
            destroy(kr);
    }
}

/* Self-tests */

#include __INC_TEST(kres)
