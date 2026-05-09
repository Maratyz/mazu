/* SPDX-License-Identifier: MIT */
/* Kernel thread framework.
 *
 * Each kthread wraps a sched_task with a managed lifecycle:
 * create -> run thread_fn -> store result -> wake stop-waiter -> terminate.
 *
 * should_stop / has_exited use __atomic acquire/release for SMP safety.
 */

#include "kthread.h"
#include <mazu/assert.h>
#include <mazu/kvalloc.h>
#include <mazu/pcpu.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/time.h>
#include "waitqueue.h"

/* Entry wrapper: runs the thread function, stores result, signals exit. */
static void kthread_entry(void *ctx)
{
    struct kthread *kt = ctx;
    assert(kt);

    kt->heartbeat_ms = time_current_ms().ms;
    kt->result = kt->thread_fn(kt->arg);
    kt->heartbeat_ms = time_current_ms().ms;

    __atomic_store_n(&kt->has_exited, true, __ATOMIC_RELEASE);
    wake_up(&kt->exit_wq, 1);

    /* Return -> sched_task_finish -> TERMINATING -> deferred free. */
}

struct kthread *kthread_create(int (*fn)(void *),
                               void *arg,
                               const char *name __unused,
                               u8 prio)
{
    assert(fn);
    assert(prio < CONFIG_SCHED_NPRIO);

    struct option_byte_array mem =
        kvalloc_alloc(sizeof(struct kthread), alignof(struct kthread));
    if (mem.is_none)
        return NULL;

    struct kthread *kt = byte_array_ptr(option_byte_array_checked(mem));
    kt->magic = KTHREAD_MAGIC;
    kt->thread_fn = fn;
    kt->arg = arg;
    kt->result = 0;
    kt->should_stop = false;
    kt->has_exited = false;
    kt->heartbeat_ms = time_current_ms().ms;
    init_waitqueue_head(&kt->exit_wq);

    struct result r = sched_create_task_prio(kthread_entry, kt, prio);
    if (r.is_error) {
        kvalloc_free(byte_array_new((void *) kt, sizeof(*kt)));
        return NULL;
    }

    return kt;
}

int kthread_stop(struct kthread *kt)
{
    assert(kt);

    /* Signal the thread to stop. */
    __atomic_store_n(&kt->should_stop, true, __ATOMIC_RELEASE);

    /* The thread function is responsible for checking kthread_should_stop()
     * and exiting promptly.  If the thread is blocked on a wait queue, it
     * should include kthread_should_stop() in its wait condition (e.g.,
     * wait_event(wq, cond || kthread_should_stop())).  This matches Linux
     * semantics: kthread_stop only sets the flag - the thread must notice.
     */

    /* Wait for the thread to exit. */
    wait_event(kt->exit_wq, __atomic_load_n(&kt->has_exited, __ATOMIC_ACQUIRE));

    /* Synchronize with kthread_entry's wake_up: take the wq lock to
     * guarantee wake_up (which holds this lock) has fully returned on
     * the remote CPU before freeing kt.  Without this, kthread_entry
     * could still be inside spin_unlock_irqrestore(&kt->exit_wq.lock)
     * when the memory is freed - a use-after-free.
     */
    u64 flags = spin_lock_irqsave(&kt->exit_wq.lock);
    spin_unlock_irqrestore(&kt->exit_wq.lock, flags);

    int ret = kt->result;
    kvalloc_free(byte_array_new((void *) kt, sizeof(*kt)));
    return ret;
}

bool kthread_should_stop(void)
{
    struct sched_task *td = get_pcpu()->curthread;
    assert(td);
    struct kthread *kt = td->context;
    if (!kt)
        return false;
    /* Type-safety: verify context is actually a kthread, not some other
     * subsystem's context pointer (prevents UB from blind cast).
     */
    if (kt->magic != KTHREAD_MAGIC)
        return false;
    kt->heartbeat_ms = time_current_ms().ms;
    return __atomic_load_n(&kt->should_stop, __ATOMIC_ACQUIRE);
}

/* Self-tests */

#include __INC_TEST(kthread)
