/* SPDX-License-Identifier: MIT */
/* Futex implementation: hash table of per-bucket wait lists.
 *
 * Each hash bucket holds a list of waiters keyed by user virtual address.
 * FUTEX_WAIT reads the user word under the bucket lock to prevent lost
 * wakeups (the lock serializes the read-check-sleep sequence with FUTEX_WAKE).
 *
 * 64 buckets with a multiplicative hash; sufficient for an embedded kernel
 * where the total number of concurrent futex addresses is small.
 */

#include "futex.h"
#include <mazu/eventlog.h>
#include <mazu/init.h>
#include <mazu/initgraph.h>
#include <mazu/list.h>
#include <mazu/print.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/spinlock.h>
#include <mazu/uaccess.h>
#include "../lockdep.h"
#include "../proc/signal.h"

#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1U << FUTEX_HASH_BITS)

struct futex_bucket;

struct futex_waiter {
    ptr key; /* user virtual address */
    struct sched_task *task;
    struct futex_bucket *bucket; /* current wait bucket; updated on requeue */
    struct list_head node;
};

struct futex_bucket {
    spinlock_t lock;
    struct list_head waiters;
};

struct futex_cleanup_ctx {
    struct futex_waiter *waiter;
};

static struct futex_bucket futex_table[FUTEX_HASH_SIZE];

static void futex_wait_cleanup(struct sched_task *task, void *ctx_ptr)
{
    struct futex_cleanup_ctx *ctx = ctx_ptr;
    assert(task);
    assert(ctx);
    assert(ctx->waiter);
    assert(ctx->waiter->bucket);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&ctx->waiter->bucket->lock);
    if (ctx->waiter->node.next != &ctx->waiter->node)
        list_del_init(&ctx->waiter->node);
    spin_unlock_irqrestore(&ctx->waiter->bucket->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
}

static u32 futex_hash(ptr addr)
{
    /* Multiplicative hash: strip low 2 bits (u32 alignment), mix. */
    u64 h = (u64) addr >> 2;
    h *= 0x45d9f3bUL;
    h ^= h >> 16;
    return (u32) (h & (FUTEX_HASH_SIZE - 1));
}

void futex_init(void)
{
    for (u32 i = 0; i < FUTEX_HASH_SIZE; i++) {
        futex_table[i].lock = (spinlock_t) SPINLOCK_INITIALIZER;
        list_init(&futex_table[i].waiters);
    }
}

i64 futex_wait(ptr uaddr, u32 expected)
{
    DEBUG_ASSERT(!in_interrupt_context());

    u32 idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    struct futex_waiter w = {
        .key = uaddr,
        .task = sched_current_task(),
        .bucket = b,
    };
    struct futex_cleanup_ctx cleanup = {
        .waiter = &w,
    };
    list_init(&w.node);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);

    /* Read the user futex word under the bucket lock.  The lock
     * serializes this read with futex_wake, preventing lost wakeups.
     */
    u32 val;
    i64 rc = copy_from_user(&val, uaddr, sizeof(val));
    if (rc < 0) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i64) EFAULT;
    }

    if (val != expected) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i64) EAGAIN;
    }

    /* Value matches: enqueue and block. */
    list_add_tail(&b->waiters, &w.node);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, futex_wait_cleanup, &cleanup);

    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    sched_yield_trap();

    /* Clean up: remove from bucket if still linked (defensive). */
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    flags = spin_lock_irqsave(&w.bucket->lock);
    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&w.bucket->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    if (signal_pending_current())
        return -(i64) EINTR;
    return 0;
}

i64 futex_wake(ptr uaddr, u32 max_wake)
{
    u32 idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];
    u32 woken = 0;

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);
    struct futex_waiter *w;

    list_for_each_entry_safe (&b->waiters, w, struct futex_waiter, node) {
        if (woken >= max_wake)
            break;
        if (w->key != uaddr)
            continue;
        if (w->task->state != TD_STATE_BLOCKED)
            continue;

        list_del_init(&w->node);
        sched_wake_ready(w->task);
        woken++;
    }

    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    return (i64) woken;
}

i64 futex_cmp_requeue(ptr uaddr1,
                      u32 expected,
                      ptr uaddr2,
                      u32 nr_wake,
                      u32 nr_requeue)
{
    u32 idx1 = futex_hash(uaddr1);
    u32 idx2 = futex_hash(uaddr2);
    struct futex_bucket *b1 = &futex_table[idx1];
    struct futex_bucket *b2 = &futex_table[idx2];

    /* Lock both buckets in address order to avoid ABBA deadlock. */
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags;
    if (idx1 <= idx2) {
        flags = spin_lock_irqsave(&b1->lock);
        if (idx1 != idx2)
            spin_lock(&b2->lock);
    } else {
        flags = spin_lock_irqsave(&b2->lock);
        spin_lock(&b1->lock);
    }

    /* Check user word atomically with the bucket lock held. */
    u32 val;
    i64 rc = copy_from_user(&val, uaddr1, sizeof(val));
    if (rc < 0) {
        rc = -(i64) EFAULT;
        goto unlock;
    }
    if (val != expected) {
        rc = -(i64) EAGAIN;
        goto unlock;
    }

    u32 woken = 0;
    u32 requeued = 0;
    struct futex_waiter *w;

    list_for_each_entry_safe (&b1->waiters, w, struct futex_waiter, node) {
        if (w->key != uaddr1)
            continue;
        if (w->task->state != TD_STATE_BLOCKED)
            continue;

        if (woken < nr_wake) {
            list_del_init(&w->node);
            sched_wake_ready(w->task);
            woken++;
        } else if (requeued < nr_requeue) {
            /* Move waiter from bucket 1 to bucket 2.
             * Keep cleanup active and retarget it to the new bucket so the
             * blocked task removes itself from the correct wait list on wake.
             */
            list_del_init(&w->node);
            w->key = uaddr2;
            w->bucket = b2;
            list_add_tail(&b2->waiters, &w->node);
            requeued++;
        } else {
            break;
        }
    }

    rc = (i64) woken + (i64) requeued;

unlock:
    if (idx1 <= idx2) {
        if (idx1 != idx2)
            spin_unlock(&b2->lock);
        spin_unlock_irqrestore(&b1->lock, flags);
    } else {
        spin_unlock(&b1->lock);
        spin_unlock_irqrestore(&b2->lock, flags);
    }
    lockdep_release(LOCK_LEVEL_WAITQ);
    return rc;
}

i64 futex_lock_pi(ptr uaddr)
{
    DEBUG_ASSERT(!in_interrupt_context());

    u32 idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    struct futex_waiter w = {
        .key = uaddr,
        .task = sched_current_task(),
        .bucket = b,
    };
    struct futex_cleanup_ctx cleanup = {
        .waiter = &w,
    };
    list_init(&w.node);

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);

    /* Read the futex word: 0 = unlocked, non-zero = owner TID. */
    u32 val;
    i64 rc = copy_from_user(&val, uaddr, sizeof(val));
    if (rc < 0) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i64) EFAULT;
    }

    if (val == 0) {
        /* Unlocked: try to acquire by writing our TID. */
        u32 tid = (u32) sched_current_id();
        rc = copy_to_user(uaddr, &tid, sizeof(tid));
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return (rc < 0) ? -(i64) EFAULT : 0;
    }

    /* Already held: enqueue and block.  futex_unlock_pi wakes the
     * highest-priority waiter (direct handover), preventing unbounded
     * inversion at unlock time.  Full owner-boost PI (boosting the
     * holder while a higher-priority task waits) requires integrating
     * with the kernel pi_mutex; deferred until user-space pthread_mutex
     * needs it.
     */
    list_add_tail(&b->waiters, &w.node);
    sched_set_task_state(w.task, TD_STATE_BLOCKED);
    sched_set_block_cleanup(w.task, futex_wait_cleanup, &cleanup);

    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    sched_yield_trap();

    /* Clean up on wake. */
    lockdep_acquire(LOCK_LEVEL_WAITQ);
    flags = spin_lock_irqsave(&w.bucket->lock);
    list_del_init(&w.node);
    sched_clear_block_cleanup(w.task);
    if (w.task->state == TD_STATE_BLOCKED)
        sched_set_task_state(w.task, TD_STATE_RUNNING);
    spin_unlock_irqrestore(&w.bucket->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);

    if (signal_pending_current())
        return -(i64) EINTR;
    return 0;
}

i64 futex_unlock_pi(ptr uaddr)
{
    u32 idx = futex_hash(uaddr);
    struct futex_bucket *b = &futex_table[idx];

    lockdep_acquire(LOCK_LEVEL_WAITQ);
    u64 flags = spin_lock_irqsave(&b->lock);

    /* Verify caller owns the futex. */
    u32 val;
    i64 rc = copy_from_user(&val, uaddr, sizeof(val));
    if (rc < 0) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i64) EFAULT;
    }

    u32 tid = (u32) sched_current_id();
    if (val != tid) {
        spin_unlock_irqrestore(&b->lock, flags);
        lockdep_release(LOCK_LEVEL_WAITQ);
        return -(i64) EPERM;
    }

    /* Find highest-priority waiter and hand off ownership. */
    struct futex_waiter *best = NULL;
    struct futex_waiter *w;
    list_for_each_entry_safe (&b->waiters, w, struct futex_waiter, node) {
        if (w->key != uaddr)
            continue;
        if (w->task->state != TD_STATE_BLOCKED)
            continue;
        if (!best || w->task->td_prio > best->task->td_prio)
            best = w;
    }

    if (best) {
        /* Transfer ownership: write waiter's TID to futex word.
         * If the write fails, leave the waiter blocked and the futex
         * word unchanged -- caller gets -EFAULT.
         */
        u32 new_tid = (u32) best->task->id;
        i64 wr = copy_to_user(uaddr, &new_tid, sizeof(new_tid));
        if (wr < 0) {
            spin_unlock_irqrestore(&b->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return -(i64) EFAULT;
        }
        list_del_init(&best->node);
        sched_wake_ready(best->task);
    } else {
        /* No waiters: clear the futex word. */
        u32 zero = 0;
        i64 wr = copy_to_user(uaddr, &zero, sizeof(zero));
        if (wr < 0) {
            spin_unlock_irqrestore(&b->lock, flags);
            lockdep_release(LOCK_LEVEL_WAITQ);
            return -(i64) EFAULT;
        }
    }

    spin_unlock_irqrestore(&b->lock, flags);
    lockdep_release(LOCK_LEVEL_WAITQ);
    return 0;
}

/* Walk the robust futex list of a dying process and unlock each entry.
 *
 * For each entry in the linked list:
 *   1. Compute the futex word address: entry_ptr + robust_futex_offset.
 *   2. Set FUTEX_OWNER_DIED in the futex word so the next acquirer
 *      knows the lock was orphaned and shared state may be inconsistent.
 *   3. Wake one waiter so a blocked task can acquire the orphaned futex.
 *
 * Also handles robust_pending (an entry being locked but not yet linked
 * into the list).
 *
 * Bounded to ROBUST_LIST_LIMIT iterations to prevent a corrupted user
 * list from wedging the kernel.
 */
#define ROBUST_LIST_LIMIT 2048
#define FUTEX_OWNER_DIED 0x40000000U

static void futex_handle_robust_entry(ptr entry, i32 futex_offset)
{
    /* Checked pointer arithmetic: reject overflow/underflow before
     * computing the futex address.  A corrupted user list could craft
     * entry + offset to wrap around and land on an unrelated address.
     * Unsigned arithmetic avoids signed-overflow UB on corrupted input.
     */
    uptr uentry = (uptr) entry;
    uptr futex_uaddr;
    if (futex_offset >= 0) {
        uptr off = (uptr) futex_offset;
        if (off > U64_MAX - uentry)
            return;
        futex_uaddr = uentry + off;
    } else {
        uptr off = (uptr) (-(i64) futex_offset);
        if (off > uentry)
            return;
        futex_uaddr = uentry - off;
    }
    ptr futex_addr = (ptr) futex_uaddr;

    /* Reject misaligned futex word -- u32 must be naturally aligned. */
    if ((uptr) futex_addr & (sizeof(u32) - 1))
        return;

    /* Validate the futex address before touching it. */
    if (!user_addr_writable(futex_addr, sizeof(u32)))
        return;

    /* Write FUTEX_OWNER_DIED into the futex word, clearing the TID.
     * The flag signals to the next acquirer that the previous holder
     * crashed and shared state may need recovery.
     */
    u32 val = FUTEX_OWNER_DIED;
    i64 rc = copy_to_user(futex_addr, &val, sizeof(val));
    if (rc < 0)
        return;

    /* Wake one waiter. */
    futex_wake(futex_addr, 1);
}

void futex_exit_robust_list(struct proc *p)
{
    if (!p || p->robust_list_head == 0)
        return;

    ptr head = p->robust_list_head;
    i32 offset = p->robust_futex_offset;
    ptr pending = p->robust_pending;

    /* Handle the pending entry first (may not be in the list). */
    if (pending != 0)
        futex_handle_robust_entry(pending, offset);

    /* Walk the linked list.  Each entry's first sizeof(ptr) bytes
     * is a pointer to the next entry (like Linux robust_list.next).
     * The list is circular: terminates when we see the head again.
     */
    ptr entry = head;
    for (u32 i = 0; i < ROBUST_LIST_LIMIT; i++) {
        /* Read the next pointer from user space. */
        ptr next;
        i64 rc = copy_from_user(&next, entry, sizeof(next));
        if (rc < 0)
            break;

        /* Process this entry (skip if it was already handled as pending). */
        if (entry != head && entry != pending)
            futex_handle_robust_entry(entry, offset);

        entry = next;

        /* Circular list: stop when we return to the head. */
        if (entry == head)
            break;
    }

    /* Clear the process's robust list state. */
    p->robust_list_head = 0;
    p->robust_futex_offset = 0;
    p->robust_pending = 0;
}

static void futex_init_hook(u32 lifecycle_flag __unused)
{
    futex_init();
}
INIT_TASK("futex",
          futex_init_hook,
          INIT_REQUIRES(INITGRAPH_STAGE_SCHED),
          INIT_ENTAILS_NONE,
          INIT_FLAG_PRIMARY);

#include __INC_TEST(futex)
