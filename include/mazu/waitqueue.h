/* SPDX-License-Identifier: MIT */
/* Public type definition for wait_queue_head.
 *
 * The struct is exposed here so other headers (notably mazu/sched.h) can embed
 * it in larger objects without circular includes. The full blocking API
 * (prepare_to_wait / finish_wait / wake_up / the wait_event macro) lives in
 * kernel/sched/waitqueue.h alongside the implementation, since callers of those
 * interfaces are kernel-side only.
 */

#ifndef MAZU_WAITQUEUE_H
#define MAZU_WAITQUEUE_H

#include <mazu/list.h>
#include <mazu/spinlock.h>

struct wait_queue_head {
    spinlock_t lock;
    struct list_head head;
};

#define WAIT_QUEUE_HEAD_INITIALIZER(name)                                    \
    {                                                                        \
        .lock = SPINLOCK_INITIALIZER, .head = { &(name).head, &(name).head } \
    }

#endif /* MAZU_WAITQUEUE_H */
