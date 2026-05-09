/* SPDX-License-Identifier: MIT */
/* Kernel-side handle table for PSE51 synchronization objects.
 *
 * User-space identifies sync objects by integer handles (returned from
 * SYS_MUTEX_INIT, SYS_COND_INIT, SYS_SEM_INIT).  The kernel maps
 * handles to pool-allocated objects.
 *
 * Pool sizes are deliberately small (bare-metal RT: bounded resources).
 */

#ifndef MAZU_SYNC_HANDLE_H
#define MAZU_SYNC_HANDLE_H

#include <mazu/base.h>
#include "barrier.h"
#include "condvar.h"
#include "mutex.h"
#include "rwlock.h"
#include "semaphore.h"

#define SYNC_MAX_MUTEXES 16
#define SYNC_MAX_CONDVARS 16
#define SYNC_MAX_SEMAPHORES 16
#define SYNC_MAX_BARRIERS 8
#define SYNC_MAX_RWLOCKS 16

struct sync_mutex_slot {
    struct pi_mutex mtx;
    struct proc *owner;
    u32 owner_gen;
    bool in_use;
};

struct sync_condvar_slot {
    struct condvar cv;
    struct proc *owner;
    u32 owner_gen;
    bool in_use;
};

struct sync_sem_slot {
    struct semaphore sem;
    struct proc *owner;
    u32 owner_gen;
    bool in_use;
};

struct sync_barrier_slot {
    struct barrier bar;
    struct proc *owner;
    u32 owner_gen;
    bool in_use;
};

struct sync_rwlock_slot {
    struct rwlock rw;
    struct proc *owner;
    u32 owner_gen;
    bool in_use;
};

/* Initialize the sync handle table.  Called once during boot. */
void sync_handle_init(void);

/* All alloc functions take the owning process for isolation.
 * All get functions validate the caller matches the owner.
 */
i32 sync_mutex_alloc(struct proc *owner);
struct pi_mutex *sync_mutex_get(i32 handle, struct proc *caller);
void sync_mutex_free(i32 handle, struct proc *caller);

i32 sync_condvar_alloc(struct proc *owner);
struct condvar *sync_condvar_get(i32 handle, struct proc *caller);
void sync_condvar_free(i32 handle, struct proc *caller);

i32 sync_sem_alloc(struct proc *owner, i32 initial_count);
struct semaphore *sync_sem_get(i32 handle, struct proc *caller);
void sync_sem_free(i32 handle, struct proc *caller);

i32 sync_barrier_alloc(struct proc *owner, u32 count);
struct barrier *sync_barrier_get(i32 handle, struct proc *caller);
void sync_barrier_free(i32 handle, struct proc *caller);

i32 sync_rwlock_alloc(struct proc *owner);
struct rwlock *sync_rwlock_get(i32 handle, struct proc *caller);
void sync_rwlock_free(i32 handle, struct proc *caller);

/* Release all sync handles owned by a process.  Called during proc_exit. */
void sync_handle_teardown_proc(struct proc *p);

#endif /* MAZU_SYNC_HANDLE_H */
