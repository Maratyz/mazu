/* SPDX-License-Identifier: MIT */
/* Signal infrastructure for PSE51.
 *
 * Signal types and state struct are defined in <mazu/proc.h>.
 * This header provides the signal API functions.
 */

#ifndef MAZU_SIGNAL_H
#define MAZU_SIGNAL_H

#include <isr.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/uaccess.h>

/* Signal numbers (POSIX subset). */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17

/* Signal disposition values. */
#define SIG_DFL ((sig_handler_fn_t) 0)
#define SIG_IGN ((sig_handler_fn_t) 1)

/* Initialize signal state for a new process. */
void signal_init(struct signal_state *ss);

/* Enqueue a signal to a process.  Returns 0 or -errno. */
i32 signal_send(struct proc *p, i32 signo);

/* Check and deliver pending signals on return-to-user.
 * Modifies the trap frame to divert to the signal handler.
 * Returns true if a signal was delivered.
 */
bool signal_deliver(struct proc *p, struct trap_frame *tf);

/* Restore trap frame after signal handler returns (SYS_SIGRETURN). */
i32 signal_return(struct proc *p, struct trap_frame *tf);

/* Handle a faulting return through the synthetic sigreturn trampoline. */
bool signal_handle_trampoline_fault(struct proc *p, struct trap_frame *tf);

/* Per-process synthetic sigreturn return address.
 * This uses the existing unmapped guard page below the user stack.
 */
static inline ptr signal_trampoline_pc(struct proc *p)
{
    if (!p)
        return (ptr) 0;
    return (ptr) (p->va_stack_top - USER_STACK_SIZE - PAGE_SIZE);
}

/* Lockless best-effort check for deliverable signals.
 * Used to skip the full signal-delivery lock path on the common
 * no-signal return-to-user fast path.
 */
static inline bool signal_has_deliverable(struct proc *p)
{
    if (!p)
        return false;
    u32 pending = __atomic_load_n(&p->sig_state.pending, __ATOMIC_RELAXED);
    u32 blocked = __atomic_load_n(&p->sig_state.blocked, __ATOMIC_RELAXED);
    return (pending & ~blocked) != 0;
}

/* Return true if the current task's process has deliverable signals.
 * Safe to call from any context; returns false for kernel tasks (no proc).
 */
static inline bool signal_pending_current(void)
{
    struct sched_task *td = sched_current_task();
    return td && td->proc && signal_has_deliverable(td->proc);
}

#endif /* MAZU_SIGNAL_H */
