/* SPDX-License-Identifier: MIT */
/* Handle table for PSE51 synchronization objects.
 *
 * Each slot tracks the owning process (pointer + generation) for
 * cross-process isolation.  get() returns NULL if the caller is not
 * the owner, preventing handle-guessing attacks.
 */

#include "sync_handle.h"
#include <mazu/init.h>
#include <mazu/proc.h>
#include <mazu/spinlock.h>

static struct sync_mutex_slot mutex_pool[SYNC_MAX_MUTEXES];
static struct sync_condvar_slot condvar_pool[SYNC_MAX_CONDVARS];
static struct sync_sem_slot sem_pool[SYNC_MAX_SEMAPHORES];
static struct sync_barrier_slot barrier_pool[SYNC_MAX_BARRIERS];
static struct sync_rwlock_slot rwlock_pool[SYNC_MAX_RWLOCKS];
static spinlock_t sync_lock = SPINLOCK_INITIALIZER;

/* Ownership check: slot must be in_use and caller must match owner+generation.
 * NULL owner (kernel tasks) matches NULL caller.
 */
static inline bool sync_owner_ok(struct proc *owner,
                                 u32 gen,
                                 struct proc *caller)
{
    if (!owner && !caller)
        return true; /* kernel-internal handle */
    return owner == caller && caller && gen == caller->generation;
}

void sync_handle_init(void)
{
    for (i32 i = 0; i < SYNC_MAX_MUTEXES; i++)
        mutex_pool[i].in_use = false;
    for (i32 i = 0; i < SYNC_MAX_CONDVARS; i++)
        condvar_pool[i].in_use = false;
    for (i32 i = 0; i < SYNC_MAX_SEMAPHORES; i++)
        sem_pool[i].in_use = false;
    for (i32 i = 0; i < SYNC_MAX_BARRIERS; i++)
        barrier_pool[i].in_use = false;
    for (i32 i = 0; i < SYNC_MAX_RWLOCKS; i++)
        rwlock_pool[i].in_use = false;
}

/* --- Mutex --- */

i32 sync_mutex_alloc(struct proc *owner)
{
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_MUTEXES; i++) {
        if (!mutex_pool[i].in_use) {
            mutex_pool[i].in_use = true;
            mutex_pool[i].owner = owner;
            mutex_pool[i].owner_gen = owner ? owner->generation : 0;
            pi_mutex_init(&mutex_pool[i].mtx);
            spin_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
    return -(i32) EAGAIN;
}

struct pi_mutex *sync_mutex_get(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_MUTEXES)
        return NULL;
    struct sync_mutex_slot *s = &mutex_pool[handle];
    if (!s->in_use || !sync_owner_ok(s->owner, s->owner_gen, caller))
        return NULL;
    return &s->mtx;
}

void sync_mutex_free(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_MUTEXES)
        return;
    u64 flags = spin_lock_irqsave(&sync_lock);
    struct sync_mutex_slot *s = &mutex_pool[handle];
    if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, caller)) {
        s->in_use = false;
        s->owner = NULL;
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

/* --- Condvar --- */

i32 sync_condvar_alloc(struct proc *owner)
{
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_CONDVARS; i++) {
        if (!condvar_pool[i].in_use) {
            condvar_pool[i].in_use = true;
            condvar_pool[i].owner = owner;
            condvar_pool[i].owner_gen = owner ? owner->generation : 0;
            condvar_init(&condvar_pool[i].cv);
            spin_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
    return -(i32) EAGAIN;
}

struct condvar *sync_condvar_get(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_CONDVARS)
        return NULL;
    struct sync_condvar_slot *s = &condvar_pool[handle];
    if (!s->in_use || !sync_owner_ok(s->owner, s->owner_gen, caller))
        return NULL;
    return &s->cv;
}

void sync_condvar_free(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_CONDVARS)
        return;
    u64 flags = spin_lock_irqsave(&sync_lock);
    struct sync_condvar_slot *s = &condvar_pool[handle];
    if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, caller)) {
        s->in_use = false;
        s->owner = NULL;
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

/* --- Semaphore --- */

i32 sync_sem_alloc(struct proc *owner, i32 initial_count)
{
    if (initial_count < 0)
        return -(i32) EINVAL;
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_SEMAPHORES; i++) {
        if (!sem_pool[i].in_use) {
            sem_pool[i].in_use = true;
            sem_pool[i].owner = owner;
            sem_pool[i].owner_gen = owner ? owner->generation : 0;
            sem_init(&sem_pool[i].sem, initial_count);
            spin_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
    return -(i32) EAGAIN;
}

struct semaphore *sync_sem_get(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_SEMAPHORES)
        return NULL;
    struct sync_sem_slot *s = &sem_pool[handle];
    if (!s->in_use || !sync_owner_ok(s->owner, s->owner_gen, caller))
        return NULL;
    return &s->sem;
}

void sync_sem_free(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_SEMAPHORES)
        return;
    u64 flags = spin_lock_irqsave(&sync_lock);
    struct sync_sem_slot *s = &sem_pool[handle];
    if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, caller)) {
        s->in_use = false;
        s->owner = NULL;
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

/* --- Barrier --- */

i32 sync_barrier_alloc(struct proc *owner, u32 count)
{
    if (count == 0)
        return -(i32) EINVAL;
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_BARRIERS; i++) {
        if (!barrier_pool[i].in_use) {
            barrier_pool[i].in_use = true;
            barrier_pool[i].owner = owner;
            barrier_pool[i].owner_gen = owner ? owner->generation : 0;
            barrier_init(&barrier_pool[i].bar, count);
            spin_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
    return -(i32) EAGAIN;
}

struct barrier *sync_barrier_get(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_BARRIERS)
        return NULL;
    struct sync_barrier_slot *s = &barrier_pool[handle];
    if (!s->in_use || !sync_owner_ok(s->owner, s->owner_gen, caller))
        return NULL;
    return &s->bar;
}

void sync_barrier_free(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_BARRIERS)
        return;
    u64 flags = spin_lock_irqsave(&sync_lock);
    struct sync_barrier_slot *s = &barrier_pool[handle];
    if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, caller)) {
        s->in_use = false;
        s->owner = NULL;
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

/* --- Rwlock --- */

i32 sync_rwlock_alloc(struct proc *owner)
{
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_RWLOCKS; i++) {
        if (!rwlock_pool[i].in_use) {
            rwlock_pool[i].in_use = true;
            rwlock_pool[i].owner = owner;
            rwlock_pool[i].owner_gen = owner ? owner->generation : 0;
            rwlock_init(&rwlock_pool[i].rw);
            spin_unlock_irqrestore(&sync_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
    return -(i32) EAGAIN;
}

struct rwlock *sync_rwlock_get(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_RWLOCKS)
        return NULL;
    struct sync_rwlock_slot *s = &rwlock_pool[handle];
    if (!s->in_use || !sync_owner_ok(s->owner, s->owner_gen, caller))
        return NULL;
    return &s->rw;
}

void sync_rwlock_free(i32 handle, struct proc *caller)
{
    if (handle < 0 || handle >= SYNC_MAX_RWLOCKS)
        return;
    u64 flags = spin_lock_irqsave(&sync_lock);
    struct sync_rwlock_slot *s = &rwlock_pool[handle];
    if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, caller)) {
        s->in_use = false;
        s->owner = NULL;
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

void sync_handle_teardown_proc(struct proc *p)
{
    u64 flags = spin_lock_irqsave(&sync_lock);
    for (i32 i = 0; i < SYNC_MAX_MUTEXES; i++) {
        struct sync_mutex_slot *s = &mutex_pool[i];
        if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, p)) {
            s->in_use = false;
            s->owner = NULL;
        }
    }
    for (i32 i = 0; i < SYNC_MAX_CONDVARS; i++) {
        struct sync_condvar_slot *s = &condvar_pool[i];
        if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, p)) {
            s->in_use = false;
            s->owner = NULL;
        }
    }
    for (i32 i = 0; i < SYNC_MAX_SEMAPHORES; i++) {
        struct sync_sem_slot *s = &sem_pool[i];
        if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, p)) {
            s->in_use = false;
            s->owner = NULL;
        }
    }
    for (i32 i = 0; i < SYNC_MAX_BARRIERS; i++) {
        struct sync_barrier_slot *s = &barrier_pool[i];
        if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, p)) {
            s->in_use = false;
            s->owner = NULL;
        }
    }
    for (i32 i = 0; i < SYNC_MAX_RWLOCKS; i++) {
        struct sync_rwlock_slot *s = &rwlock_pool[i];
        if (s->in_use && sync_owner_ok(s->owner, s->owner_gen, p)) {
            s->in_use = false;
            s->owner = NULL;
        }
    }
    spin_unlock_irqrestore(&sync_lock, flags);
}

static void sync_handle_boot_init(u32 flag __unused)
{
    sync_handle_init();
}
DEFINE_INIT_HOOK(sync_handle_boot_init, INIT_LEVEL_SUBSYS, INIT_FLAG_PRIMARY);
