/* SPDX-License-Identifier: MIT */
/* Signal delivery for PSE51.
 *
 * Signal delivery model:
 * - Signals are pending bits, not queued (standard POSIX behavior).
 * - Delivery happens on trap exit (return-to-user path).
 * - Handler invocation: save current trap frame on the user stack,
 *   set up execution to jump to the handler, sys_sigreturn restores.
 * - SIGKILL and SIGTERM with default disposition terminate the process.
 */

#include "signal.h"
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/eventlog.h>
#include <mazu/print.h>
#include <mazu/sched.h>
#include <mazu/uaccess.h>

#define SIGNAL_FRAME_MAGIC 0x53494746U

void signal_init(struct signal_state *ss)
{
    ss->pending = 0;
    ss->blocked = 0;
    ss->frame_top = (ptr) 0;
    ss->frame_prev = (ptr) 0;
    ss->frame_cookie = 0;
    ss->frame_prev_cookie = 0;
    for (i32 i = 0; i < SIG_MAX; i++) {
        ss->actions[i].handler = SIG_DFL;
        ss->actions[i].sa_mask = 0;
        ss->actions[i].sa_flags = 0;
    }
}

static inline bool sig_valid(i32 signo)
{
    return signo > 0 && signo < SIG_MAX;
}

static inline u32 sig_bit(i32 signo)
{
    return 1U << signo;
}

static inline bool signal_restore_tf_valid(struct proc *p,
                                           const struct trap_frame *tf)
{
    if (!p || !tf)
        return false;
    if ((tf->sstatus & (SSTATUS_SPP | SSTATUS_SUM)) != 0)
        return false;
    if (!proc_vma_check_access(p, (ptr) tf->sepc, 1, VMA_PERM_EXEC))
        return false;
    if (!proc_vma_check_access(p, (ptr) tf->sp, 1, VMA_PERM_WRITE))
        return false;
    return true;
}

static inline void signal_restore_tf(struct trap_frame *dst,
                                     const struct trap_frame *src)
{
    *dst = *src;
    dst->scause = 0;
    dst->stval = 0;
    dst->sstatus = SSTATUS_SPIE;
}

/* Interrupt a task that is blocked in a wait state so it can observe
 * a newly posted signal at the earliest opportunity.
 *
 * TD_STATE_SLEEPING (nanosleep): sched_wake_sleeping cancels the sleep
 * callout, removes from sleep_list, and enqueues as READY — all under
 * sched_lock, which serializes against the normal sleep callout wake.
 *
 * TD_STATE_BLOCKED / TD_STATE_SEM_WAIT (sync primitives): we cannot
 * safely call sched_wake_ready because the normal wake path (sem_post,
 * condvar_signal, etc.) may concurrently wake and enqueue the same task,
 * causing double-enqueue corruption.  Instead, the pending-signal bit
 * is already set in sig_state.pending, and every interruptible wait loop
 * checks signal_pending_current() at its next iteration.  For timed waits
 * the callout will fire and re-enter the loop.  For untimed waits the
 * signal is delivered when the primitive's normal wake path fires (post,
 * signal, broadcast, unlock).  SIGKILL bypasses this entirely via the
 * dedicated proc_exit + TD_STATE_TERMINATING path.
 */
static void signal_interrupt_task(struct sched_task *td)
{
    if (!td)
        return;

    if (td->state == TD_STATE_SLEEPING)
        sched_wake_sleeping(td);
}

i32 signal_send(struct proc *p, i32 signo)
{
    if (!p || !sig_valid(signo))
        return -(i32) EINVAL;

    u64 flags = proc_sig_lock_irqsave(p);

    /* SIGKILL cannot be masked or ignored. */
    if (signo == SIGKILL) {
        p->sig_state.pending |= sig_bit(SIGKILL);
        proc_sig_unlock_irqrestore(p, flags);

        /* Snapshot p->task under proc_table_lock — same lock that
         * proc_exit uses to detach p->task.
         */
        u64 tflags = proc_table_lock_irqsave();
        struct sched_task *td = p->task;
        proc_table_unlock_irqrestore(tflags);

        proc_exit(p, -SIGKILL);
        if (td)
            sched_set_task_state(td, TD_STATE_TERMINATING);
        return 0;
    }

    p->sig_state.pending |= sig_bit(signo);
    proc_sig_unlock_irqrestore(p, flags);

    /* Snapshot p->task under proc_table_lock.  proc_exit detaches
     * p->task under this same lock, so reading here guarantees the
     * pointer is either valid (task still alive) or NULL (already
     * detached).  We must not dereference td after releasing the lock
     * without first checking it is non-NULL.
     */
    u64 tflags = proc_table_lock_irqsave();
    struct sched_task *td = p->task;
    proc_table_unlock_irqrestore(tflags);

    signal_interrupt_task(td);

    return 0;
}

/* Saved signal context pushed onto the user stack. */
struct signal_frame {
    u32 magic;
    u32 cookie;
    struct trap_frame saved_tf;
    u32 saved_blocked;
    ptr prev_frame;
    u32 prev_cookie;
    i32 signo;
};

bool signal_deliver(struct proc *p, struct trap_frame *tf)
{
    if (!p)
        return false;

    u64 flags = proc_sig_lock_irqsave(p);
    u32 deliverable = p->sig_state.pending & ~p->sig_state.blocked;
    if (deliverable == 0) {
        proc_sig_unlock_irqrestore(p, flags);
        return false;
    }

    /* Find lowest-numbered pending unblocked signal. */
    i32 signo = 0;
    for (i32 i = 1; i < SIG_MAX; i++) {
        if (deliverable & sig_bit(i)) {
            signo = i;
            break;
        }
    }
    if (signo == 0) {
        proc_sig_unlock_irqrestore(p, flags);
        return false;
    }

    p->sig_state.pending &= ~sig_bit(signo);
    sig_handler_fn_t handler = p->sig_state.actions[signo].handler;

    if (handler == SIG_IGN) {
        proc_sig_unlock_irqrestore(p, flags);
        return false;
    }

    if (handler == SIG_DFL) {
        struct sched_task *td = p->task;
        proc_sig_unlock_irqrestore(p, flags);
        /* Default: terminate (except SIGCHLD which is ignored). */
        if (signo == SIGCHLD)
            return false;
        pr_info(STR("signal: pid=%hu terminated by signal %d\n"), p->pid,
                signo);
        proc_exit(p, -signo);
        if (td)
            sched_set_task_state(td, TD_STATE_TERMINATING);
        return true;
    }

    /* Custom handler: push signal frame onto user stack. */
    u64 sp = tf->sp;
    sp -= sizeof(struct signal_frame);
    sp &= ~0xFUL;

    struct signal_frame frame = {
        .magic = SIGNAL_FRAME_MAGIC,
        .cookie = p->sig_state.frame_cookie + 1,
        .saved_tf = *tf,
        .saved_blocked = p->sig_state.blocked,
        .prev_frame = p->sig_state.frame_top,
        .prev_cookie = p->sig_state.frame_cookie,
        .signo = signo,
    };

    p->sig_state.blocked |=
        p->sig_state.actions[signo].sa_mask | sig_bit(signo);
    p->sig_state.frame_prev = frame.prev_frame;
    p->sig_state.frame_prev_cookie = frame.prev_cookie;
    p->sig_state.frame_top = (ptr) sp;
    p->sig_state.frame_cookie = frame.cookie;
    proc_sig_unlock_irqrestore(p, flags);

    i64 rc = copy_to_user((ptr) sp, &frame, sizeof(frame));
    if (rc < 0) {
        struct sched_task *td = p->task;
        pr_info(STR("signal: pid=%hu stack fault during signal %d\n"), p->pid,
                signo);
        proc_exit(p, -signo);
        if (td)
            sched_set_task_state(td, TD_STATE_TERMINATING);
        return true;
    }

    /* Redirect to handler: a0=signo, a1=frame_ptr, sp=new stack. */
    tf->sepc = (u64) (uptr) handler;
    tf->a0 = (u64) signo;
    tf->a1 = (u64) sp;
    tf->sp = sp;
    tf->ra = (u64) signal_trampoline_pc(p);

    return true;
}

i32 signal_return(struct proc *p, struct trap_frame *tf)
{
    if (!p)
        return -(i32) EPERM;

    ptr frame_ptr = (ptr) tf->a0;
    struct signal_frame frame;
    i64 rc = copy_from_user(&frame, frame_ptr, sizeof(frame));
    if (rc < 0)
        return -(i32) EFAULT;

    /* Validate the saved trap frame BEFORE committing any signal state
     * changes.  A malicious handler could supply a frame with valid
     * magic/cookie but an invalid saved_tf; committing first would
     * corrupt the frame stack and signal mask.
     */
    if (!signal_restore_tf_valid(p, &frame.saved_tf))
        return -(i32) EPERM;

    u64 flags = proc_sig_lock_irqsave(p);
    bool frame_ok = frame.magic == SIGNAL_FRAME_MAGIC &&
                    frame_ptr == p->sig_state.frame_top &&
                    frame.cookie == p->sig_state.frame_cookie;
    if (!frame_ok) {
        proc_sig_unlock_irqrestore(p, flags);
        return -(i32) EPERM;
    }
    p->sig_state.frame_top = frame.prev_frame;
    p->sig_state.frame_cookie = frame.prev_cookie;
    p->sig_state.frame_prev = (ptr) 0;
    p->sig_state.frame_prev_cookie = 0;
    p->sig_state.blocked = frame.saved_blocked;
    proc_sig_unlock_irqrestore(p, flags);

    signal_restore_tf(tf, &frame.saved_tf);
    return 0;
}

bool signal_handle_trampoline_fault(struct proc *p, struct trap_frame *tf)
{
    if (!p || !tf || tf->sepc != (u64) signal_trampoline_pc(p))
        return false;

    tf->a0 = tf->sp;
    return signal_return(p, tf) == 0;
}
