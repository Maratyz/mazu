/* SPDX-License-Identifier: MIT */
/* Kernel thread framework.
 *
 * Replaces ad-hoc task spawning with managed lifecycle:
 *   kthread_create  - allocate kthread, spawn scheduler task
 *   kthread_stop    - request stop, wait for exit, return result
 *   kthread_should_stop - polled by the thread function
 *
 * Lifetime: kthread struct is freed only by kthread_stop (or leaked
 * if nobody calls stop).  The entry wrapper never frees it.
 */

#ifndef MAZU_KTHREAD_H
#define MAZU_KTHREAD_H

#include <mazu/base.h>
#include "waitqueue.h"

/* Magic number for struct kthread (ASCII 'kthr'). */
#define KTHREAD_MAGIC 0x6B746872U

struct kthread {
    u32 magic; /* KTHREAD_MAGIC - validated in kthread_should_stop */
    int (*thread_fn)(void *);
    void *arg;
    int result;
    struct wait_queue_head exit_wq;
    volatile bool should_stop;
    volatile bool has_exited;
    u16 task_id;
    u64 heartbeat_ms;
};

struct kthread *kthread_create(int (*fn)(void *),
                               void *arg,
                               const char *name,
                               u8 prio);
int kthread_stop(struct kthread *kt);
bool kthread_should_stop(void);

#endif /* MAZU_KTHREAD_H */
