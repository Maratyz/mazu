/* SPDX-License-Identifier: MIT */
/* Syscall dispatch for user-space processes.
 *
 * Called from trap_dispatch() when scause == ECALL_U.  The trapframe's
 * a7 selects the syscall number; a0-a5 carry arguments.  Return value
 * is placed in a0 by the caller.
 *
 * Security invariant: all user pointers must pass through copy_from_user /
 * copy_to_user.  Direct dereference of user pointers is forbidden.
 * Invalid syscall numbers return -ENOSYS; this is not a security hole
 * because user-space can already invoke any ecall.
 */

#include <mazu/assert.h>
#include <mazu/eventlog.h>
#include <mazu/ipi.h>
#include <mazu/klog.h>
#include <mazu/kvalloc.h>
#include <mazu/pcpu.h>
#include <mazu/posix_time.h>
#include <mazu/print.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/spawn.h>
#include <mazu/string.h>
#include <mazu/syscall.h>
#include <mazu/sysconf.h>
#include <mazu/time.h>
#include <mazu/uaccess.h>
#include <mazu/vfs.h>

#include "../ipc/mqueue.h"
#include "../sync/futex.h"
#include "../sync/mutex.h"
#include "../sync/sync_handle.h"
#include "../timer/posix_timer.h"
#include "elf64.h"
#include "pipe.h"
#include "signal.h"

/* The per-process allow-list (struct proc::syscall_allow[2]) is two u64 words
 * indexed by (nr / 64). Adding a 65th syscall without widening the array
 * silently writes past the end during dispatch.
 */
static_assert(SYS_NR <= 128, "syscall_allow[2] only covers nrs 0..127");

/* Copy a user-space path into kpath[257].  Returns the kernel str on success,
 * or sets *err to a negative errno and returns an empty str.
 */
static struct str copy_user_path(ptr upath, sz pathlen, char *kpath, i64 *err)
{
    if (pathlen <= 0 || pathlen > 256) {
        *err = -(i64) EINVAL;
        return (struct str) {0};
    }
    i64 rc = copy_from_user(kpath, upath, pathlen);
    if (rc < 0) {
        *err = rc;
        return (struct str) {0};
    }
    *err = 0;
    return str_new(kpath, pathlen);
}

/* Validate only the numeric fd range.  Open-state checks happen under
 * p->fd_lock at the callsite.
 */
static inline bool validate_fd_number(i32 fd)
{
    return fd >= 0 && fd < PROC_FD_MAX;
}

static i32 find_free_fd_locked(struct proc *p)
{
    assert(p);
    for (i32 i = 0; i < PROC_FD_MAX; i++) {
        if (!p->fd_table[i].is_open)
            return i;
    }
    return -1;
}

static i64 sys_exit(struct trap_frame *tf, struct sched_task *td)
{
    if (!td)
        return -(i64) EPERM;
    i32 code = (i32) tf->a0;
    struct proc *p = td->proc;
    u16 pid = p ? p->pid : 0;
    if (p)
        proc_exit(p, code);
    sched_set_task_state(td, TD_STATE_TERMINATING);
    pr_info(STR("process pid=%hu exited with code %d\n"), pid, code);
    return 0;
}

static i64 sys_write(struct trap_frame *tf, struct sched_task *td)
{
    i32 fd = (i32) tf->a0;
    ptr ubuf = (ptr) tf->a1;
    sz len = (sz) tf->a2;

    struct proc *p = td->proc;
    if (!p || !validate_fd_number(fd))
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[fd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }

    /* Pipe dispatch: release fd_lock before blocking I/O.
     * Bump the pipe's writer refcount first so concurrent close() on
     * the last FD cannot free the pipe during the operation.
     */
    if (p->fd_table[fd].is_pipe) {
        struct pipe *pipe = p->fd_table[fd].pipe;
        bool is_write = !p->fd_table[fd].pipe_read_end;
        if (!is_write) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) EBADF;
        }
        u64 pf = spin_lock_irqsave(&pipe->lock);
        pipe->writers++;
        spin_unlock_irqrestore(&pipe->lock, pf);
        proc_fd_unlock_irqrestore(p, fd_flags);

        if (len <= 0) {
            pipe_close_write(pipe);
            return 0;
        }
        if (len > PIPE_BUF_SIZE)
            len = PIPE_BUF_SIZE;

        /* Copy the entire payload into a kernel buffer first, then hand
         * it to pipe_write as a single atomic call.  Splitting into
         * multiple pipe_write calls would let concurrent writers
         * interleave their data, breaking POSIX PIPE_BUF atomicity.
         */
        char kbuf[PIPE_BUF_SIZE];
        i64 rc = copy_from_user(kbuf, ubuf, len);
        if (rc < 0) {
            pipe_close_write(pipe);
            return rc;
        }
        i64 written = pipe_write(pipe, kbuf, len);
        pipe_close_write(pipe);
        return written;
    }

    if (len <= 0) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return 0;
    }
    if (len > 4096)
        len = 4096;

    /* fd 1/2 = stdout/stderr -> console output. */
    if (fd == PROC_FD_STDOUT || fd == PROC_FD_STDERR) {
        char kbuf[256];
        sz total = 0;
        while (total < len) {
            sz chunk = len - total;
            if (chunk > (sz) sizeof(kbuf))
                chunk = (sz) sizeof(kbuf);
            i64 rc = copy_from_user(kbuf, ubuf + total, chunk);
            if (rc < 0) {
                proc_fd_unlock_irqrestore(p, fd_flags);
                return rc;
            }
            print_str((struct str) {.dat = kbuf, .len = chunk});
            total += chunk;
        }
        proc_fd_unlock_irqrestore(p, fd_flags);
        return total;
    }

    char kbuf[256];
    sz total = 0;
    sz base_off = p->fd_table[fd].offset;
    while (total < len) {
        sz chunk = len - total;
        if (chunk > (sz) sizeof(kbuf))
            chunk = (sz) sizeof(kbuf);
        i64 rc = copy_from_user(kbuf, ubuf + total, chunk);
        if (rc < 0) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return rc;
        }
        struct byte_view bv = byte_view_new(kbuf, chunk);
        struct result_sz wres =
            vfs_write(&p->fd_table[fd].file, bv, base_off + total);
        if (wres.is_error) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) wres.code;
        }
        sz written = result_sz_checked(wres);
        if (written == 0)
            break;
        total += written;
    }
    p->fd_table[fd].offset = base_off + total;
    proc_fd_unlock_irqrestore(p, fd_flags);
    return total;
}

static i64 sys_read(struct trap_frame *tf, struct sched_task *td)
{
    i32 fd = (i32) tf->a0;
    ptr ubuf = (ptr) tf->a1;
    sz len = (sz) tf->a2;

    struct proc *p = td->proc;
    if (!p || !validate_fd_number(fd))
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[fd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }

    /* Pipe dispatch: release fd_lock before blocking I/O.
     * Bump the pipe's reader refcount first so concurrent close() on
     * the last FD cannot free the pipe during the operation.
     */
    if (p->fd_table[fd].is_pipe) {
        struct pipe *pipe = p->fd_table[fd].pipe;
        bool is_read = p->fd_table[fd].pipe_read_end;
        if (!is_read) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) EBADF;
        }
        u64 pf = spin_lock_irqsave(&pipe->lock);
        pipe->readers++;
        spin_unlock_irqrestore(&pipe->lock, pf);
        proc_fd_unlock_irqrestore(p, fd_flags);

        if (len <= 0) {
            pipe_close_read(pipe);
            return 0;
        }
        if (len > 4096)
            len = 4096;

        /* Validate the entire user buffer before consuming pipe bytes.
         * pipe_read advances the head irreversibly, so a late EFAULT
         * from copy_to_user would lose data from a non-seekable FD.
         * Must check writability (PTE_W), not just accessibility -
         * a read-only mapping passes user_addr_valid but faults on write.
         */
        if (!user_addr_writable(ubuf, len)) {
            pipe_close_read(pipe);
            return -(i64) EFAULT;
        }

        char kbuf[256];
        sz total = 0;
        while (total < len) {
            sz chunk = len - total;
            if (chunk > (sz) sizeof(kbuf))
                chunk = (sz) sizeof(kbuf);
            i64 got = pipe_read(pipe, kbuf, chunk);
            if (got < 0) {
                pipe_close_read(pipe);
                return total > 0 ? (i64) total : got;
            }
            if (got == 0)
                break;
            i64 rc = copy_to_user(ubuf + total, kbuf, (sz) got);
            if (rc < 0) {
                pipe_close_read(pipe);
                return total > 0 ? (i64) total : rc;
            }
            total += (sz) got;
            if ((sz) got < chunk)
                break; /* short read: don't block again */
        }
        pipe_close_read(pipe);
        return (i64) total;
    }

    if (fd == PROC_FD_STDIN) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return 0;
    }

    if (len <= 0) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return 0;
    }
    if (len > 4096)
        len = 4096;

    char kbuf[256];
    sz total = 0;
    sz base_off = p->fd_table[fd].offset;
    while (total < len) {
        sz chunk = len - total;
        if (chunk > (sz) sizeof(kbuf))
            chunk = (sz) sizeof(kbuf);
        struct byte_buf bb = byte_buf_new(kbuf, 0, chunk);
        struct result_sz rres =
            vfs_read(&p->fd_table[fd].file, &bb, base_off + total);
        if (rres.is_error) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) rres.code;
        }
        sz got = result_sz_checked(rres);
        if (got == 0)
            break;
        i64 rc = copy_to_user(ubuf + total, kbuf, got);
        if (rc < 0) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return rc;
        }
        total += got;
    }
    p->fd_table[fd].offset = base_off + total;
    proc_fd_unlock_irqrestore(p, fd_flags);
    return total;
}

static i64 sys_open(struct trap_frame *tf, struct sched_task *td)
{
    ptr upath = (ptr) tf->a0;
    sz pathlen = (sz) tf->a1;

    struct proc *p = td->proc;
    if (!p)
        return -(i64) EPERM;

    /* Preserve EMFILE semantics before touching user memory. */
    u64 fd_flags = proc_fd_lock_irqsave(p);
    i32 fd = find_free_fd_locked(p);
    proc_fd_unlock_irqrestore(p, fd_flags);
    if (fd < 0)
        return -(i64) EMFILE;

    i64 perr;
    char kpath[257];
    struct str path = copy_user_path(upath, pathlen, kpath, &perr);
    if (perr < 0)
        return perr;

    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error)
        return -(i64) fres.code;

    fd_flags = proc_fd_lock_irqsave(p);
    fd = find_free_fd_locked(p);
    if (fd < 0) {
        struct vfs_file file = result_vfs_file_checked(fres);
        proc_fd_unlock_irqrestore(p, fd_flags);
        vfs_close(&file);
        return -(i64) EMFILE;
    }

    p->fd_table[fd] = (struct proc_fd) {
        .is_open = true,
        .is_seekable = true,
        .file = result_vfs_file_checked(fres),
    };
    proc_fd_unlock_irqrestore(p, fd_flags);
    return fd;
}

static i64 sys_close(struct trap_frame *tf, struct sched_task *td)
{
    i32 fd = (i32) tf->a0;

    struct proc *p = td->proc;
    if (!p || !validate_fd_number(fd))
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[fd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }

    proc_close_fd_locked(p, fd);
    proc_fd_unlock_irqrestore(p, fd_flags);
    return 0;
}

static i64 sys_stat(struct trap_frame *tf, struct sched_task *td)
{
    ptr upath = (ptr) tf->a0;
    sz pathlen = (sz) tf->a1;
    ptr ubuf = (ptr) tf->a2;

    (void) td;

    i64 perr;
    char kpath[257];
    struct str path = copy_user_path(upath, pathlen, kpath, &perr);
    if (perr < 0)
        return perr;

    struct result_vfs_stat st = vfs_stat(path);
    if (st.is_error)
        return -(i64) st.code;

    struct vfs_stat kstat = result_vfs_stat_checked(st);
    return copy_to_user(ubuf, &kstat, sizeof(kstat));
}

static i64 sys_yield(struct trap_frame *tf, struct sched_task *td)
{
    (void) tf;
    if (!td)
        return -(i64) EPERM;
    sched_set_task_state(td, TD_STATE_YIELDING);
    return 0;
}

static i64 sys_time(struct trap_frame *tf, struct sched_task *td)
{
    (void) tf;
    (void) td;
    return (i64) time_current_ms().ms;
}

/* Maximum binary size for sys_spawn (same as shell: 8 KiB). */
#define SPAWN_BUF_MAX 8192

/* Copy file actions array from user-space into kernel buffer.
 * For SPAWN_FA_OPEN actions, also copies the path string and replaces the
 * user pointer with the kernel pointer.  Returns 0 on success.
 */
static i64 copy_file_actions(ptr ufa_ptr,
                             sz fa_count,
                             struct spawn_file_action *kfa,
                             char (*kpaths)[SPAWN_FA_PATH_MAX])
{
    sz fa_size = fa_count * sizeof(struct spawn_file_action);
    i64 rc = copy_from_user(kfa, ufa_ptr, fa_size);
    if (rc < 0)
        return rc;

    for (sz i = 0; i < fa_count; i++) {
        if (kfa[i].type == SPAWN_FA_OPEN) {
            if (kfa[i].pathlen == 0 || kfa[i].pathlen > SPAWN_FA_PATH_MAX)
                return -(i64) EINVAL;
            rc = copy_from_user(kpaths[i], (ptr) kfa[i].path, kfa[i].pathlen);
            if (rc < 0)
                return rc;
            /* Replace user pointer with kernel pointer. */
            kfa[i].path = (u64) kpaths[i];
        }
    }
    return 0;
}

static i64 sys_spawn(struct trap_frame *tf, struct sched_task *td)
{
    ptr upath = (ptr) tf->a0;
    sz pathlen = (sz) tf->a1;
    ptr ufa_ptr = (ptr) tf->a2;   /* file_actions array, or 0 */
    sz fa_count = (sz) tf->a3;    /* number of file actions */
    ptr uattr_ptr = (ptr) tf->a4; /* spawn_attr pointer, or 0 */
    struct proc *parent = td->proc;

    /* Validate file action count early. */
    if (ufa_ptr && fa_count > SPAWN_FA_MAX)
        return -(i64) EINVAL;
    if (!ufa_ptr)
        fa_count = 0;

    /* Copy path from user-space. */
    i64 perr;
    char kpath[257];
    struct str path = copy_user_path(upath, pathlen, kpath, &perr);
    if (perr < 0)
        return perr;

    /* Copy file actions from user-space (if any). kpaths must outlive kfa
     * because copy_file_actions stores &kpaths[i] into kfa[i].path, which
     * is later dereferenced by spawn_apply_file_actions below.
     */
    struct spawn_file_action kfa[SPAWN_FA_MAX];
    char kpaths[SPAWN_FA_MAX][SPAWN_FA_PATH_MAX];
    if (fa_count > 0) {
        i64 fa_rc = copy_file_actions(ufa_ptr, fa_count, kfa, kpaths);
        if (fa_rc < 0)
            return fa_rc;
    }

    /* Copy spawn attributes from user-space (if any). */
    struct spawn_attr kattr;
    bool has_attr = false;
    if (uattr_ptr) {
        i64 attr_rc = copy_from_user(&kattr, uattr_ptr, sizeof(kattr));
        if (attr_rc < 0)
            return attr_rc;
        has_attr = true;
    }

    /* Validate: must be a regular file within size limit. */
    struct result_vfs_stat st = vfs_stat(path);
    if (st.is_error)
        return -(i64) st.code;
    struct vfs_stat kstat = result_vfs_stat_checked(st);
    if (kstat.type != VFS_TYPE_FILE)
        return -(i64) ENOEXEC;
    if (kstat.size == 0 || kstat.size > SPAWN_BUF_MAX)
        return -(i64) ENOEXEC;

    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error)
        return -(i64) fres.code;
    struct vfs_file f = result_vfs_file_checked(fres);

    /* Allocate a kernel buffer for the binary. */
    struct option_byte_array ba = kvalloc_alloc(SPAWN_BUF_MAX, 8);
    if (ba.is_none) {
        vfs_close(&f);
        return -(i64) ENOMEM;
    }
    struct byte_array buf_ba = option_byte_array_checked(ba);
    byte *buf = buf_ba.dat;

    struct byte_buf bb = byte_buf_new(buf, 0, SPAWN_BUF_MAX);
    struct result_sz rr = vfs_read(&f, &bb, 0);
    vfs_close(&f);

    if (rr.is_error) {
        kvalloc_free(buf_ba);
        return -(i64) rr.code;
    }
    sz nread = result_sz_checked(rr);
    if (nread == 0 || nread != kstat.size) {
        kvalloc_free(buf_ba);
        return -(i64) EIO;
    }

    /* Determine scheduling priority (default or from attr). */
    u8 child_prio = SCHED_PRIO_NORMAL;
    if (has_attr) {
        i32 attr_rc = spawn_apply_attr(NULL, &kattr, &child_prio);
        if (attr_rc < 0) {
            kvalloc_free(buf_ba);
            return (i64) attr_rc;
        }
    }

    /* Allocate child process. */
    struct proc *child = proc_alloc();
    if (!child) {
        kvalloc_free(buf_ba);
        return -(i64) ENOMEM;
    }
    child->parent_pid = parent->pid;

    /* Copy binary name into child proc. */
    sz namelen = pathlen < 31 ? pathlen : 31;
    for (sz i = 0; i < namelen; i++)
        child->name[i] = kpath[i];
    child->name[namelen] = '\0';

    /* Load binary: try ELF first, fall back to flat. */
    struct byte_view bv = byte_view_new(buf, nread);
    ptr entry;
    struct result lr;

    if (nread >= (sz) sizeof(struct elf64_hdr) &&
        elf64_is_valid((struct elf64_hdr *) buf)) {
        entry = ((struct elf64_hdr *) buf)->entry;
        /* Validate entry point is within the child's VA window. */
        if ((u64) entry < (u64) child->va_code_base ||
            (u64) entry >= (u64) child->va_stack_top) {
            kvalloc_free(buf_ba);
            proc_free(child);
            return -(i64) ENOEXEC;
        }
        lr = proc_load_elf(child, bv);
    } else {
        entry = (ptr) child->va_code_base;
        lr = proc_load_flat(child, bv);
    }
    if (lr.is_error) {
        kvalloc_free(buf_ba);
        proc_free(child);
        return -(i64) lr.code;
    }

    kvalloc_free(buf_ba);

    /* Inherit parent's FD table so file actions (especially DUP2) can
     * reference descriptors that are open in the parent.
     *
     * Pipe FDs: inherit with refcount bump (pipes have proper lifecycle).
     * Console FDs (stdin/stdout/stderr without VFS backing): inherit as-is.
     * VFS file FDs: do NOT inherit - the VFS layer has no refcounting,
     * so sharing a struct vfs_file across processes leads to use-after-free
     * when either side closes the descriptor.  Callers that need file
     * redirection must use SPAWN_FA_OPEN file actions instead.
     *
     * Lock ordering: parent fd_lock first (read), child fd_lock second
     * (write, uncontended - child is EMBRYO, invisible to other harts).
     */
    {
        u64 pf = proc_fd_lock_irqsave(parent);
        u64 cf = proc_fd_lock_irqsave(child);
        for (i32 i = 0; i < PROC_FD_MAX; i++) {
            if (!parent->fd_table[i].is_open)
                continue;
            if (parent->fd_table[i].is_pipe) {
                child->fd_table[i] = parent->fd_table[i];
                struct pipe *pipe = child->fd_table[i].pipe;
                /* IRQs already disabled by parent fd_lock; plain
                 * spin_lock avoids redundant irqsave/restore.
                 */
                spin_lock(&pipe->lock);
                if (child->fd_table[i].pipe_read_end)
                    pipe->readers++;
                else
                    pipe->writers++;
                spin_unlock(&pipe->lock);
            } else if (!parent->fd_table[i].is_seekable) {
                /* Console FD (no VFS backing): safe to copy by value. */
                child->fd_table[i] = parent->fd_table[i];
            }
            /* VFS file FDs (is_seekable && !is_pipe): skip - no refcount,
             * sharing leads to use-after-free on close.
             */
        }
        proc_fd_unlock_irqrestore(child, cf);
        proc_fd_unlock_irqrestore(parent, pf);
    }

    /* Apply file actions to child's FD table (child still in EMBRYO). */
    if (fa_count > 0) {
        i32 fa_rc = spawn_apply_file_actions(child, kfa, fa_count);
        if (fa_rc < 0) {
            proc_free(child);
            return (i64) fa_rc;
        }
    }

    /* Transition to RUNNING before enqueue: sched_create_user_task
     * enqueues the task immediately, so another CPU could schedule
     * and even terminate it before returning.  Setting RUNNING after
     * enqueue would corrupt a concurrent ZOMBIE transition.
     */
    proc_set_state(child, PROC_STATE_RUNNING);

    struct result tr = sched_create_user_task(child, entry, child_prio);
    if (tr.is_error) {
        proc_set_state(child, PROC_STATE_ZOMBIE);
        proc_free(child);
        return -(i64) tr.code;
    }
    pr_info(STR("sys_spawn: pid=%hu spawned \"%s\" as pid=%hu\n"),
            (u32) parent->pid, path, (u32) child->pid);
    return (i64) child->pid;
}

static i64 sys_wait(struct trap_frame *tf, struct sched_task *td)
{
    ptr ustatus = (ptr) tf->a0;
    struct proc *p = td->proc;

    u16 child_pid;
    i32 exit_code;
    i32 rc = proc_wait_child(p, &child_pid, &exit_code);
    if (rc < 0)
        return (i64) rc;

    /* Copy exit code to user-space if pointer is non-NULL. */
    if (ustatus) {
        i64 err = copy_to_user(ustatus, &exit_code, sizeof(exit_code));
        if (err < 0)
            return err;
    }
    return (i64) child_pid;
}

static i64 sys_getpid(struct trap_frame *tf, struct sched_task *td)
{
    (void) tf;
    struct proc *p = td->proc;
    return p ? (i64) p->pid : 0;
}

static i64 sys_getppid(struct trap_frame *tf, struct sched_task *td)
{
    (void) tf;
    struct proc *p = td->proc;
    return p ? (i64) p->parent_pid : 0;
}

/* Bump pipe refcount after duplicating a pipe FD.  Caller holds fd_lock. */
static void pipe_bump_refcount(struct proc_fd *f)
{
    if (!f->is_pipe)
        return;
    u64 pf = spin_lock_irqsave(&f->pipe->lock);
    if (f->pipe_read_end)
        f->pipe->readers++;
    else
        f->pipe->writers++;
    spin_unlock_irqrestore(&f->pipe->lock, pf);
}

static i64 sys_dup(struct trap_frame *tf, struct sched_task *td)
{
    i32 oldfd = (i32) tf->a0;
    struct proc *p = td->proc;
    if (!p || !validate_fd_number(oldfd))
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[oldfd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }

    i32 newfd = find_free_fd_locked(p);
    if (newfd < 0) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EMFILE;
    }

    p->fd_table[newfd] = p->fd_table[oldfd];
    p->fd_table[newfd].is_dup = true;
    pipe_bump_refcount(&p->fd_table[newfd]);
    proc_fd_unlock_irqrestore(p, fd_flags);
    return newfd;
}

static i64 sys_dup2(struct trap_frame *tf, struct sched_task *td)
{
    i32 oldfd = (i32) tf->a0;
    i32 newfd = (i32) tf->a1;
    struct proc *p = td->proc;
    if (!p || !validate_fd_number(oldfd))
        return -(i64) EBADF;
    if (newfd < 0 || newfd >= PROC_FD_MAX)
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[oldfd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }
    if (oldfd == newfd) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return newfd;
    }
    if (p->fd_table[newfd].is_open)
        proc_close_fd_locked(p, newfd);
    p->fd_table[newfd] = p->fd_table[oldfd];
    p->fd_table[newfd].is_dup = true;
    pipe_bump_refcount(&p->fd_table[newfd]);
    proc_fd_unlock_irqrestore(p, fd_flags);
    return newfd;
}

static i64 sys_lseek(struct trap_frame *tf, struct sched_task *td)
{
    i32 fd = (i32) tf->a0;
    i64 offset = (i64) tf->a1;
    i32 whence = (i32) tf->a2;

    struct proc *p = td->proc;
    if (!p || !validate_fd_number(fd))
        return -(i64) EBADF;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    if (!p->fd_table[fd].is_open) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EBADF;
    }
    if (!p->fd_table[fd].is_seekable) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) ESPIPE;
    }

    sz cur = p->fd_table[fd].offset;
    sz new_off;

    switch (whence) {
    case SEEK_SET:
        if (offset < 0) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) EINVAL;
        }
        new_off = (sz) offset;
        break;
    case SEEK_CUR: {
        u64 delta = (offset < 0) ? (u64) (-(offset + 1)) + 1 : (u64) offset;
        if (offset < 0 && delta > (u64) cur) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) EINVAL;
        }
        if (offset > 0 && (u64) cur > (u64) (I64_MAX - offset)) {
            proc_fd_unlock_irqrestore(p, fd_flags);
            return -(i64) EINVAL;
        }
        new_off = cur + (sz) offset;
        break;
    }
    case SEEK_END:
        /* Needs vfs_file_size() - not yet available. */
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) ENOSYS;
    default:
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EINVAL;
    }

    /* Guard: new_off must fit in i64 for the return value. */
    if (new_off > (sz) I64_MAX) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) EINVAL;
    }

    p->fd_table[fd].offset = new_off;
    proc_fd_unlock_irqrestore(p, fd_flags);
    return (i64) new_off;
}

static i64 sys_chdir(struct trap_frame *tf, struct sched_task *td)
{
    ptr upath = (ptr) tf->a0;
    sz pathlen = (sz) tf->a1;
    struct proc *p = td->proc;

    if (pathlen >= PROC_PATH_MAX)
        return -(i64) ENAMETOOLONG;

    i64 perr;
    char kpath[257];
    struct str path = copy_user_path(upath, pathlen, kpath, &perr);
    if (perr < 0)
        return perr;

    /* Verify the path exists and is a directory. */
    struct result_vfs_stat st = vfs_stat(path);
    if (st.is_error)
        return -(i64) st.code;
    struct vfs_stat kstat = result_vfs_stat_checked(st);
    if (kstat.type != VFS_TYPE_DIR)
        return -(i64) ENOTDIR;

    u64 fd_flags = proc_fd_lock_irqsave(p);
    memcpy(p->cwd, kpath, pathlen);
    p->cwd_len = pathlen;
    p->cwd[pathlen] = '\0';
    proc_fd_unlock_irqrestore(p, fd_flags);
    return 0;
}

static inline bool futex_addr_valid(ptr uaddr)
{
    if ((u64) uaddr % sizeof(u32) != 0)
        return false;
    uptr ua = (uptr) uaddr;
    return ua >= (uptr) USER_CODE_BASE &&
           ua <= (uptr) USER_STACK_TOP - sizeof(u32);
}

static i64 sys_futex(struct trap_frame *tf, struct sched_task *td)
{
    (void) td;
    ptr uaddr = (ptr) tf->a0;
    i32 op = (i32) tf->a1;
    u32 val = (u32) tf->a2;

    if (!futex_addr_valid(uaddr))
        return -(i64) EINVAL;

    switch (op) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val);
    case FUTEX_WAKE:
        return futex_wake(uaddr, val);
    case FUTEX_CMP_REQUEUE: {
        ptr uaddr2 = (ptr) tf->a3;
        u32 nr_requeue = (u32) tf->a4;
        if (!futex_addr_valid(uaddr2))
            return -(i64) EINVAL;
        return futex_cmp_requeue(uaddr, val, uaddr2, 1, nr_requeue);
    }
    case FUTEX_LOCK_PI:
        return futex_lock_pi(uaddr);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(uaddr);
    default:
        return -(i64) EINVAL;
    }
}

static i64 sys_getcwd(struct trap_frame *tf, struct sched_task *td)
{
    ptr ubuf = (ptr) tf->a0;
    sz size = (sz) tf->a1;
    struct proc *p = td->proc;
    char cwd[PROC_PATH_MAX];
    sz needed;

    /* POSIX: size must accommodate the path plus a NUL terminator. */
    u64 fd_flags = proc_fd_lock_irqsave(p);
    needed = p->cwd_len + 1;
    if (size < needed) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        return -(i64) ERANGE;
    }
    memcpy(cwd, p->cwd, needed);
    proc_fd_unlock_irqrestore(p, fd_flags);

    i64 rc = copy_to_user(ubuf, cwd, needed);
    if (rc < 0)
        return rc;
    return (i64) needed;
}

static i64 sys_pipe(struct trap_frame *tf, struct sched_task *td)
{
    ptr ufds = (ptr) tf->a0;
    struct proc *p = td->proc;

    struct pipe *pipe = pipe_alloc();
    if (!pipe)
        return -(i64) ENOMEM;

    u64 fd_flags = proc_fd_lock_irqsave(p);

    /* Find two free FDs. */
    i32 rfd = find_free_fd_locked(p);
    i32 wfd = -1;
    if (rfd >= 0) {
        p->fd_table[rfd].is_open = true;
        wfd = find_free_fd_locked(p);
        p->fd_table[rfd].is_open = false;
    }
    if (rfd < 0 || wfd < 0) {
        proc_fd_unlock_irqrestore(p, fd_flags);
        pipe_close_read(pipe);
        pipe_close_write(pipe);
        return -(i64) EMFILE;
    }

    p->fd_table[rfd] = (struct proc_fd) {
        .is_open = true,
        .is_pipe = true,
        .pipe_read_end = true,
        .pipe = pipe,
    };
    p->fd_table[wfd] = (struct proc_fd) {
        .is_open = true,
        .is_pipe = true,
        .pipe = pipe,
    };

    proc_fd_unlock_irqrestore(p, fd_flags);

    /* Copy FD pair to user-space: fds[0] = read, fds[1] = write. */
    i32 kfds[2] = {rfd, wfd};
    i64 rc = copy_to_user(ufds, kfds, sizeof(kfds));
    if (rc < 0) {
        /* Undo: close both FDs. */
        proc_close_fd(p, rfd);
        proc_close_fd(p, wfd);
        return rc;
    }
    return 0;
}

i64 sys_sysconf_query(i64 name)
{
    switch (name) {
    case _SC_PAGE_SIZE:
        return (i64) PAGE_SIZE;
    case _SC_OPEN_MAX:
        return (i64) PROC_FD_MAX;
    case _SC_NPROCESSORS_CONF: /* fall through */
    case _SC_NPROCESSORS_ONLN:
        return (i64) nr_cpus_online;
    case _SC_PIPE_BUF:
        return (i64) PIPE_BUF_SIZE;
    case _SC_CHILD_MAX:
        return (i64) (PROC_MAX - 1); /* minus one for the parent */
    case _SC_MEMLOCK:
        return 0; /* all memory is resident; locking is implicit */
    default:
        return -(i64) EINVAL;
    }
}

/* sysconf(name): return system configuration values.
 * a0 = _SC_ name constant.  Returns the value or -EINVAL.
 */
static i64 sys_sysconf(struct trap_frame *tf, struct sched_task *td __unused)
{
    return sys_sysconf_query((i64) tf->a0);
}

/* sched_setaffinity(pid, affinity): set CPU affinity for a task.
 * a0 = pid (0 = self), a1 = affinity (-1 = any, >= 0 = pinned hart).
 * Returns 0 on success, -EINVAL for invalid hart/args, -ESRCH for bad pid.
 *
 * After storing the new affinity, sends a reschedule IPI to the hart
 * where the task last ran so the scheduler re-evaluates placement.
 */
static i64 sys_sched_setaffinity(struct trap_frame *tf, struct sched_task *td)
{
    /* Validate full register width before narrowing. */
    i64 raw_pid = (i64) tf->a0;
    i64 raw_aff = (i64) tf->a1;

    if (raw_pid < 0 || raw_pid > (i64) U16_MAX)
        return -(i64) EINVAL;
    if (raw_aff < -1 || raw_aff > (i64) I32_MAX)
        return -(i64) EINVAL;

    u16 pid = (u16) raw_pid;
    i32 affinity = (i32) raw_aff;

    /* Validate affinity: -1 (any) or valid hart index. */
    if (affinity != -1 && (u32) affinity >= nr_cpus_online)
        return -(i64) EINVAL;

    struct sched_task *target;

    if (pid == 0) {
        if (!td || !td->proc)
            return -(i64) EPERM;
        target = td;
        __atomic_store_n(&target->td_affinity, affinity, __ATOMIC_RELEASE);
    } else {
        /* Hold proc_table_lock across find + affinity write to prevent
         * proc_exit() from detaching and freeing p->task concurrently.
         */
        u64 pflags = proc_table_lock_irqsave();
        struct proc *p = proc_find_locked(pid);
        if (!p || !p->task) {
            proc_table_unlock_irqrestore(pflags);
            return -(i64) ESRCH;
        }
        target = p->task;
        __atomic_store_n(&target->td_affinity, affinity, __ATOMIC_RELEASE);
#if CONFIG_SMP
        u32 last_cpu = target->td_last_cpu;
#endif
        proc_table_unlock_irqrestore(pflags);
#if CONFIG_SMP
        if (last_cpu < MAX_CPUS && last_cpu != get_cpuid())
            ipi_send(last_cpu, IPI_SCHED);
#endif
        return 0;
    }

#if CONFIG_SMP
    {
        u32 last_cpu = target->td_last_cpu;
        if (last_cpu < MAX_CPUS && last_cpu != get_cpuid())
            ipi_send(last_cpu, IPI_SCHED);
        else
            __atomic_store_n(&get_pcpu()->need_resched, 1, __ATOMIC_RELEASE);
    }
#endif

    return 0;
}

/* sched_getaffinity(pid, uptr): get CPU affinity for a task.
 * a0 = pid (0 = self), a1 = user pointer to i32 result.
 * Returns 0 on success, copies affinity to *a1.
 * Returns -ESRCH for bad pid, -EFAULT for bad pointer, -EINVAL for bad args.
 *
 * Affinity is written to user memory to avoid ambiguity with -1 (any hart)
 * and negative errno values in the syscall return.
 */
static i64 sys_sched_getaffinity(struct trap_frame *tf, struct sched_task *td)
{
    i64 raw_pid = (i64) tf->a0;
    ptr uptr_out = (ptr) tf->a1;

    if (raw_pid < 0 || raw_pid > (i64) U16_MAX)
        return -(i64) EINVAL;

    u16 pid = (u16) raw_pid;
    i32 result;

    if (pid == 0) {
        if (!td || !td->proc)
            return -(i64) EPERM;
        result = __atomic_load_n(&td->td_affinity, __ATOMIC_ACQUIRE);
    } else {
        u64 pflags = proc_table_lock_irqsave();
        struct proc *p = proc_find_locked(pid);
        if (!p || !p->task) {
            proc_table_unlock_irqrestore(pflags);
            return -(i64) ESRCH;
        }
        result = __atomic_load_n(&p->task->td_affinity, __ATOMIC_ACQUIRE);
        proc_table_unlock_irqrestore(pflags);
    }

    return copy_to_user(uptr_out, &result, sizeof(result));
}

#if CONFIG_SCHED_DEADLINE
/* sched_setattr(uattr): set scheduling policy and parameters.
 * a0 = user pointer to struct sched_attr.
 * Returns 0 on success, -EINVAL/-EBUSY/-EFAULT on failure.
 */
static i64 sys_sched_setattr(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;

    ptr uattr = (ptr) tf->a0;
    struct sched_attr kattr;
    i64 rc = copy_from_user(&kattr, uattr, sizeof(kattr));
    if (rc < 0)
        return rc;

    if (kattr.policy == SCHED_POLICY_NORMAL) {
        sched_dl_clearattr(td);
        return 0;
    }
    if (kattr.policy != SCHED_POLICY_DEADLINE)
        return -(i64) EINVAL;

    return (i64) sched_dl_setattr(td, kattr.runtime_ns, kattr.deadline_ns,
                                  kattr.period_ns);
}

/* sched_getattr(uattr): get current scheduling policy and parameters.
 * a0 = user pointer to struct sched_attr (output).
 * Returns 0 on success, -EFAULT on bad pointer.
 */
static u64 ticks_to_ns(u64 ticks, u64 freq)
{
    return (ticks / freq) * 1000000000ULL +
           ((ticks % freq) * 1000000000ULL) / freq;
}

static i64 sys_sched_getattr(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;

    ptr uattr = (ptr) tf->a0;
    struct sched_attr kattr = {0};

#if CONFIG_SCHED_DEADLINE
    kattr.policy = td->td_policy;
    if (td->td_policy == SCHED_POLICY_DEADLINE && td->dl.dl_active) {
        u64 freq = time_get_timebase_freq();
        if (freq > 0) {
            kattr.runtime_ns = ticks_to_ns(td->dl.dl_runtime, freq);
            kattr.deadline_ns = ticks_to_ns(td->dl.dl_deadline, freq);
            kattr.period_ns = ticks_to_ns(td->dl.dl_period, freq);
        }
    }
#endif

    return copy_to_user(uattr, &kattr, sizeof(kattr));
}
#endif /* CONFIG_SCHED_DEADLINE */

/* SYS_SET_ROBUST_LIST: register the user-space robust futex list.
 * a0 = pointer to robust_list_head (first entry / self = empty)
 * a1 = byte offset from entry pointer to the futex word
 * a2 = pointer to the pending entry (0 if none)
 */
static i64 sys_set_robust_list(struct trap_frame *tf, struct sched_task *td)
{
    struct proc *p = td->proc;
    ptr head = (ptr) tf->a0;
    i32 offset = (i32) tf->a1;
    ptr pending = (ptr) tf->a2;

    /* Validate alignment: the head must be pointer-aligned (the list is
     * a linked list of pointers), and the futex offset must yield a
     * u32-aligned address when applied to any entry.
     */
    if (head && ((uptr) head & (sizeof(ptr) - 1)))
        return -(i64) EINVAL;
    if (pending && ((uptr) pending & (sizeof(ptr) - 1)))
        return -(i64) EINVAL;
    if (offset & (i32) (sizeof(u32) - 1))
        return -(i64) EINVAL;

    p->robust_list_head = head;
    p->robust_futex_offset = offset;
    p->robust_pending = pending;
    return 0;
}

/* SYS_GET_ROBUST_LIST: retrieve the current robust list registration.
 * a0 = user pointer to store the head pointer (ptr *)
 * a1 = user pointer to store the futex offset (i32 *)
 * a2 = user pointer to store the pending pointer (ptr *)
 */
static i64 sys_get_robust_list(struct trap_frame *tf, struct sched_task *td)
{
    struct proc *p = td->proc;
    ptr u_head = (ptr) tf->a0;
    ptr u_offset = (ptr) tf->a1;
    ptr u_pending = (ptr) tf->a2;
    i64 rc;

    rc =
        copy_to_user(u_head, &p->robust_list_head, sizeof(p->robust_list_head));
    if (rc < 0)
        return -(i64) EFAULT;
    rc = copy_to_user(u_offset, &p->robust_futex_offset,
                      sizeof(p->robust_futex_offset));
    if (rc < 0)
        return -(i64) EFAULT;
    rc = copy_to_user(u_pending, &p->robust_pending, sizeof(p->robust_pending));
    if (rc < 0)
        return -(i64) EFAULT;
    return 0;
}

/* --- PSE51 clock and nanosleep (item 15) --- */

static i64 sys_clock_gettime(struct trap_frame *tf,
                             struct sched_task *td __unused)
{
    i32 clk_id = (i32) tf->a0;
    ptr u_ts = (ptr) tf->a1;

    if (clk_id != CLOCK_MONOTONIC && clk_id != CLOCK_REALTIME)
        return -(i64) EINVAL;

    u64 ticks = time_rdtime();
    u64 freq = time_get_timebase_freq();
    if (freq == 0)
        return -(i64) EIO;

    struct timespec ts;
    ts.tv_sec = (i64) (ticks / freq);
    ts.tv_nsec = (i64) ((ticks % freq) * NSEC_PER_SEC / freq);

    i64 rc = copy_to_user(u_ts, &ts, sizeof(ts));
    if (rc < 0)
        return rc;
    return 0;
}

static i64 sys_clock_getres(struct trap_frame *tf,
                            struct sched_task *td __unused)
{
    i32 clk_id = (i32) tf->a0;
    ptr u_ts = (ptr) tf->a1;

    if (clk_id != CLOCK_MONOTONIC && clk_id != CLOCK_REALTIME)
        return -(i64) EINVAL;

    u64 freq = time_get_timebase_freq();
    if (freq == 0)
        return -(i64) EIO;

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (i64) (NSEC_PER_SEC / freq);
    if (ts.tv_nsec == 0)
        ts.tv_nsec = 1;

    if (u_ts) {
        i64 rc = copy_to_user(u_ts, &ts, sizeof(ts));
        if (rc < 0)
            return rc;
    }
    return 0;
}

static i64 sys_nanosleep(struct trap_frame *tf, struct sched_task *td)
{
    ptr u_req = (ptr) tf->a0;

    struct timespec req;
    i64 rc = copy_from_user(&req, u_req, sizeof(req));
    if (rc < 0)
        return rc;

    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= NSEC_PER_SEC)
        return -(i64) EINVAL;

    u64 ms = (u64) req.tv_sec * 1000 + (u64) req.tv_nsec / NSEC_PER_MSEC;
    if (ms == 0 && req.tv_nsec > 0)
        ms = 1; /* sub-millisecond: round up to one tick */

    u64 before_ms = time_current_ms().ms;
    sleep_ms(time_ms_new(ms));
    u64 elapsed_ms = time_current_ms().ms - before_ms;

    /* If a signal woke us early, report EINTR so user-space knows the
     * sleep was interrupted.  The trap exit path will deliver the signal.
     */
    if (td && td->proc &&
        (td->proc->sig_state.pending & ~td->proc->sig_state.blocked) != 0 &&
        elapsed_ms < ms)
        return -(i64) EINTR;

    return 0;
}

/* --- PSE51 memory locking -- no-ops on bare metal (item 15e) --- */

static i64 sys_mlockall(struct trap_frame *tf __unused,
                        struct sched_task *td __unused)
{
    return 0;
}

static i64 sys_munlockall(struct trap_frame *tf __unused,
                          struct sched_task *td __unused)
{
    return 0;
}

/* --- PSE51 synchronization syscalls (item 15a) --- */

static i64 sys_mutex_init_h(struct trap_frame *tf __unused,
                            struct sched_task *td __unused)
{
    i32 h = sync_mutex_alloc(td->proc);
    return (i64) h;
}

static i64 sys_mutex_lock_h(struct trap_frame *tf,
                            struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct pi_mutex *mtx = sync_mutex_get(handle, td->proc);
    if (!mtx)
        return -(i64) EINVAL;
    return (i64) pi_mutex_lock_interruptible(mtx);
}

static i64 sys_mutex_trylock_h(struct trap_frame *tf,
                               struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct pi_mutex *mtx = sync_mutex_get(handle, td->proc);
    if (!mtx)
        return -(i64) EINVAL;
    return (i64) pi_mutex_trylock(mtx);
}

static i64 sys_mutex_unlock_h(struct trap_frame *tf,
                              struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct pi_mutex *mtx = sync_mutex_get(handle, td->proc);
    if (!mtx)
        return -(i64) EINVAL;
    pi_mutex_unlock(mtx);
    return 0;
}

static i64 sys_cond_init_h(struct trap_frame *tf __unused,
                           struct sched_task *td __unused)
{
    i32 h = sync_condvar_alloc(td->proc);
    return (i64) h;
}

static i64 sys_cond_wait_h(struct trap_frame *tf,
                           struct sched_task *td __unused)
{
    i32 cv_h = (i32) tf->a0;
    i32 mtx_h = (i32) tf->a1;
    struct condvar *cv = sync_condvar_get(cv_h, td->proc);
    struct pi_mutex *mtx = sync_mutex_get(mtx_h, td->proc);
    if (!cv || !mtx)
        return -(i64) EINVAL;
    return (i64) condvar_wait(cv, mtx);
}

static i64 sys_cond_timedwait_h(struct trap_frame *tf,
                                struct sched_task *td __unused)
{
    i32 cv_h = (i32) tf->a0;
    i32 mtx_h = (i32) tf->a1;
    u64 timeout_ms = tf->a2;
    struct condvar *cv = sync_condvar_get(cv_h, td->proc);
    struct pi_mutex *mtx = sync_mutex_get(mtx_h, td->proc);
    if (!cv || !mtx)
        return -(i64) EINVAL;
    return (i64) condvar_wait_timeout(cv, mtx, time_ms_new(timeout_ms));
}

static i64 sys_cond_signal_h(struct trap_frame *tf,
                             struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct condvar *cv = sync_condvar_get(handle, td->proc);
    if (!cv)
        return -(i64) EINVAL;
    condvar_signal(cv);
    return 0;
}

static i64 sys_cond_broadcast_h(struct trap_frame *tf,
                                struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct condvar *cv = sync_condvar_get(handle, td->proc);
    if (!cv)
        return -(i64) EINVAL;
    condvar_broadcast(cv);
    return 0;
}

static i64 sys_sem_init_h(struct trap_frame *tf, struct sched_task *td __unused)
{
    i32 initial = (i32) tf->a0;
    i32 h = sync_sem_alloc(td->proc, initial);
    return (i64) h;
}

static i64 sys_sem_wait_h(struct trap_frame *tf, struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct semaphore *s = sync_sem_get(handle, td->proc);
    if (!s)
        return -(i64) EINVAL;
    return (i64) sem_wait_interruptible(s);
}

static i64 sys_sem_trywait_h(struct trap_frame *tf,
                             struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct semaphore *s = sync_sem_get(handle, td->proc);
    if (!s)
        return -(i64) EINVAL;
    return (i64) sem_trywait(s);
}

static i64 sys_sem_post_h(struct trap_frame *tf, struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct semaphore *s = sync_sem_get(handle, td->proc);
    if (!s)
        return -(i64) EINVAL;
    sem_post(s);
    return 0;
}

static i64 sys_sem_timedwait_h(struct trap_frame *tf,
                               struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    u64 timeout_ms = tf->a1;
    struct semaphore *s = sync_sem_get(handle, td->proc);
    if (!s)
        return -(i64) EINVAL;
    return (i64) sem_timedwait(s, time_ms_new(timeout_ms));
}

/* --- POSIX barriers (item 15i) --- */

static i64 sys_barrier_init_h(struct trap_frame *tf,
                              struct sched_task *td __unused)
{
    u32 count = (u32) tf->a0;
    i32 h = sync_barrier_alloc(td->proc, count);
    return (i64) h;
}

static i64 sys_barrier_wait_h(struct trap_frame *tf,
                              struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct barrier *b = sync_barrier_get(handle, td->proc);
    if (!b)
        return -(i64) EINVAL;
    return (i64) barrier_wait_interruptible(b);
}

static i64 sys_barrier_destroy_h(struct trap_frame *tf,
                                 struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct barrier *b = sync_barrier_get(handle, td->proc);
    if (!b)
        return -(i64) EINVAL;
    i32 rc = barrier_destroy(b);
    if (rc == 0)
        sync_barrier_free(handle, td->proc);
    return (i64) rc;
}

/* --- POSIX rwlocks (item 15j) --- */

static i64 sys_rwlock_init_h(struct trap_frame *tf __unused,
                             struct sched_task *td __unused)
{
    i32 h = sync_rwlock_alloc(td->proc);
    return (i64) h;
}

static i64 sys_rwlock_rdlock_h(struct trap_frame *tf,
                               struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_rdlock_interruptible(rw);
}

static i64 sys_rwlock_wrlock_h(struct trap_frame *tf,
                               struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_wrlock_interruptible(rw);
}

static i64 sys_rwlock_tryrdlock_h(struct trap_frame *tf,
                                  struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_tryrdlock(rw);
}

static i64 sys_rwlock_trywrlock_h(struct trap_frame *tf,
                                  struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_trywrlock(rw);
}

static i64 sys_rwlock_unlock_h(struct trap_frame *tf,
                               struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    rwlock_unlock(rw);
    return 0;
}

static i64 sys_rwlock_timedrdlock_h(struct trap_frame *tf,
                                    struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    u64 timeout_ms = tf->a1;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_timedrdlock(rw, time_ms_new(timeout_ms));
}

static i64 sys_rwlock_timedwrlock_h(struct trap_frame *tf,
                                    struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    u64 timeout_ms = tf->a1;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    return (i64) rwlock_timedwrlock(rw, time_ms_new(timeout_ms));
}

static i64 sys_rwlock_destroy_h(struct trap_frame *tf,
                                struct sched_task *td __unused)
{
    i32 handle = (i32) tf->a0;
    struct rwlock *rw = sync_rwlock_get(handle, td->proc);
    if (!rw)
        return -(i64) EINVAL;
    i32 rc = rwlock_destroy(rw);
    if (rc == 0)
        sync_rwlock_free(handle, td->proc);
    return (i64) rc;
}

/* --- POSIX message queues (item 15b) --- */

static i64 sys_mq_open(struct trap_frame *tf, struct sched_task *td)
{
    u32 max_msgs = (u32) tf->a0;
    sz max_msg_size = (sz) tf->a1;
    struct proc *p = td ? td->proc : NULL;
    return (i64) mqueue_open(p, max_msgs, max_msg_size);
}

static i64 sys_mq_close(struct trap_frame *tf, struct sched_task *td)
{
    i32 handle = (i32) tf->a0;
    struct proc *p = td ? td->proc : NULL;
    if (!mqueue_check_owner(handle, p))
        return -(i64) EPERM;
    return (i64) mqueue_close(handle);
}

static i64 sys_mq_send(struct trap_frame *tf, struct sched_task *td)
{
    i32 handle = (i32) tf->a0;
    struct proc *p = td ? td->proc : NULL;
    if (!mqueue_check_owner(handle, p))
        return -(i64) EPERM;
    ptr u_msg = (ptr) tf->a1;
    sz len = (sz) tf->a2;
    u32 priority = (u32) tf->a3;

    if (len <= 0 || len > MQ_MAX_MSG_SIZE)
        return -(i64) EMSGSIZE;

    u8 kbuf[MQ_MAX_MSG_SIZE];
    i64 rc = copy_from_user(kbuf, u_msg, len);
    if (rc < 0)
        return rc;

    return (i64) mqueue_send(handle, kbuf, len, priority);
}

static i64 sys_mq_receive(struct trap_frame *tf, struct sched_task *td)
{
    i32 handle = (i32) tf->a0;
    struct proc *p = td ? td->proc : NULL;
    if (!mqueue_check_owner(handle, p))
        return -(i64) EPERM;
    ptr u_buf = (ptr) tf->a1;
    sz buf_size = (sz) tf->a2;
    ptr u_prio = (ptr) tf->a3;

    if (buf_size <= 0 || buf_size > MQ_MAX_MSG_SIZE)
        return -(i64) EINVAL;

    u8 kbuf[MQ_MAX_MSG_SIZE];
    u32 prio = 0;
    i32 ret = mqueue_receive(handle, kbuf, buf_size, &prio);
    if (ret < 0)
        return (i64) ret;

    i64 rc = copy_to_user(u_buf, kbuf, (sz) ret);
    if (rc < 0)
        return rc;
    if (u_prio) {
        rc = copy_to_user(u_prio, &prio, sizeof(prio));
        if (rc < 0)
            return rc;
    }
    return (i64) ret;
}

static i64 sys_mq_timedreceive(struct trap_frame *tf, struct sched_task *td)
{
    i32 handle = (i32) tf->a0;
    struct proc *p = td ? td->proc : NULL;
    if (!mqueue_check_owner(handle, p))
        return -(i64) EPERM;
    ptr u_buf = (ptr) tf->a1;
    sz buf_size = (sz) tf->a2;
    ptr u_prio = (ptr) tf->a3;
    u64 timeout_ms = tf->a4;

    if (buf_size <= 0 || buf_size > MQ_MAX_MSG_SIZE)
        return -(i64) EINVAL;

    u8 kbuf[MQ_MAX_MSG_SIZE];
    u32 prio = 0;
    i32 ret = mqueue_timedreceive(handle, kbuf, buf_size, &prio,
                                  time_ms_new(timeout_ms));
    if (ret < 0)
        return (i64) ret;

    i64 rc = copy_to_user(u_buf, kbuf, (sz) ret);
    if (rc < 0)
        return rc;
    if (u_prio) {
        rc = copy_to_user(u_prio, &prio, sizeof(prio));
        if (rc < 0)
            return rc;
    }
    return (i64) ret;
}

/* --- PSE51 scheduling syscalls (item 17) --- */

static i64 sys_sched_get_priority_min(struct trap_frame *tf __unused,
                                      struct sched_task *td __unused)
{
    return (i64) SCHED_PRIO_IDLE;
}

static i64 sys_sched_get_priority_max(struct trap_frame *tf __unused,
                                      struct sched_task *td __unused)
{
    return (i64) (CONFIG_SCHED_NPRIO - 1);
}

static i64 sys_sched_yield_pse51(struct trap_frame *tf __unused,
                                 struct sched_task *td)
{
    if (!td)
        return -(i64) EPERM;
    sched_set_task_state(td, TD_STATE_YIELDING);
    return 0;
}

static i64 sys_sched_setparam(struct trap_frame *tf, struct sched_task *td)
{
    if (!td)
        return -(i64) EPERM;

    i32 new_prio = (i32) tf->a0;
    if (new_prio < SCHED_PRIO_IDLE || new_prio >= CONFIG_SCHED_NPRIO)
        return -(i64) EINVAL;

    /* Cannot raise above caller's own base priority (privilege bound). */
    if ((u8) new_prio > td->td_base_prio)
        return -(i64) EPERM;

    td->td_base_prio = (u8) new_prio;
    pi_mutex_refresh_prio(td);
    return 0;
}

static i64 sys_sched_getparam(struct trap_frame *tf __unused,
                              struct sched_task *td)
{
    if (!td)
        return -(i64) EPERM;
    return (i64) td->td_base_prio;
}

/* --- Signals (item 16) --- */

static i64 sys_kill_h(struct trap_frame *tf, struct sched_task *td __unused)
{
    u16 pid = (u16) tf->a0;
    i32 signo = (i32) tf->a1;

    struct proc *target = proc_find(pid);
    if (!target)
        return -(i64) ESRCH;

    if (signo == 0)
        return 0; /* existence check only */

    return (i64) signal_send(target, signo);
}

static i64 sys_sigaction_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;

    i32 signo = (i32) tf->a0;
    sig_handler_fn_t handler = (sig_handler_fn_t) (uptr) tf->a1;
    u32 sa_mask = (u32) tf->a2;

    if (signo <= 0 || signo >= SIG_MAX || signo == SIGKILL)
        return -(i64) EINVAL;

    struct proc *p = td->proc;
    u64 flags = proc_sig_lock_irqsave(p);
    sig_handler_fn_t old = p->sig_state.actions[signo].handler;
    p->sig_state.actions[signo].handler = handler;
    p->sig_state.actions[signo].sa_mask = sa_mask;
    proc_sig_unlock_irqrestore(p, flags);

    return (i64) (uptr) old;
}

static i64 sys_sigreturn_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    return (i64) signal_return(td->proc, tf);
}

/* --- Thread management (item 15d) --- */

/* Thread creation is deferred: struct proc currently tracks a single p->task.
 * Creating a second thread overwrites that pointer, breaking signal delivery,
 * process exit, and every operation that walks p->task.  Until the proc model
 * supports a per-process task list (depends on item 21b VA isolation), this
 * syscall returns -ENOSYS.
 */
static i64 sys_thread_create_h(struct trap_frame *tf __unused,
                               struct sched_task *td __unused)
{
    return -(i64) ENOSYS;
}

static i64 sys_thread_join_h(struct trap_frame *tf __unused,
                             struct sched_task *td __unused)
{
    return -(i64) ENOSYS;
}

static i64 sys_thread_detach_h(struct trap_frame *tf __unused,
                               struct sched_task *td __unused)
{
    return -(i64) ENOSYS;
}

static i64 sys_thread_exit_h(struct trap_frame *tf __unused,
                             struct sched_task *td __unused)
{
    return -(i64) ENOSYS;
}

static i64 sys_thread_self_h(struct trap_frame *tf __unused,
                             struct sched_task *td)
{
    if (!td)
        return -(i64) EPERM;
    return (i64) td->id;
}

/* --- Interval timers (item 15f) --- */

static i64 sys_timer_create_h(struct trap_frame *tf __unused,
                              struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    return (i64) posix_timer_create(td->proc);
}

static i64 sys_timer_settime_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    i32 handle = (i32) tf->a0;
    u64 value_ms = tf->a1;
    u64 interval_ms = tf->a2;
    return (i64) posix_timer_settime(handle, td->proc, value_ms, interval_ms);
}

static i64 sys_timer_delete_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    return (i64) posix_timer_delete((i32) tf->a0, td->proc);
}

static i64 sys_timer_gettime_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    return (i64) posix_timer_gettime((i32) tf->a0, td->proc);
}

static i64 sys_timer_getoverrun_h(struct trap_frame *tf, struct sched_task *td)
{
    if (!td || !td->proc)
        return -(i64) EPERM;
    return (i64) posix_timer_getoverrun((i32) tf->a0, td->proc);
}

typedef i64 (*syscall_fn_t)(struct trap_frame *tf, struct sched_task *td);

struct syscall_entry {
    syscall_fn_t handler;
    u16 flags;
};

static const struct syscall_entry syscall_table[SYS_NR] = {
    [SYS_OPEN] = {sys_open, SYSCALL_F_NEEDS_PROC},
    [SYS_CLOSE] = {sys_close, SYSCALL_F_NEEDS_PROC},
    [SYS_READ] = {sys_read, SYSCALL_F_NEEDS_PROC},
    [SYS_WRITE] = {sys_write, SYSCALL_F_NEEDS_PROC},
    [SYS_STAT] = {sys_stat, 0},
    [SYS_EXIT] = {sys_exit, 0},
    [SYS_YIELD] = {sys_yield, 0},
    [SYS_TIME] = {sys_time, 0},
    [SYS_SPAWN] = {sys_spawn, SYSCALL_F_NEEDS_PROC},
    [SYS_WAIT] = {sys_wait, SYSCALL_F_NEEDS_PROC},
    [SYS_GETPID] = {sys_getpid, SYSCALL_F_NEEDS_PROC},
    [SYS_GETPPID] = {sys_getppid, SYSCALL_F_NEEDS_PROC},
    [SYS_DUP] = {sys_dup, SYSCALL_F_NEEDS_PROC},
    [SYS_DUP2] = {sys_dup2, SYSCALL_F_NEEDS_PROC},
    [SYS_LSEEK] = {sys_lseek, SYSCALL_F_NEEDS_PROC},
    [SYS_CHDIR] = {sys_chdir, SYSCALL_F_NEEDS_PROC},
    [SYS_GETCWD] = {sys_getcwd, SYSCALL_F_NEEDS_PROC},
    [SYS_FUTEX] = {sys_futex, SYSCALL_F_NEEDS_PROC},
    [SYS_PIPE] = {sys_pipe, SYSCALL_F_NEEDS_PROC},
    [SYS_SYSCONF] = {sys_sysconf, 0},
    [SYS_SCHED_SETAFFINITY] = {sys_sched_setaffinity, 0},
    [SYS_SCHED_GETAFFINITY] = {sys_sched_getaffinity, 0},
#if CONFIG_SCHED_DEADLINE
    [SYS_SCHED_SETATTR] = {sys_sched_setattr, 0},
    [SYS_SCHED_GETATTR] = {sys_sched_getattr, 0},
#endif
    [SYS_SET_ROBUST_LIST] = {sys_set_robust_list, SYSCALL_F_NEEDS_PROC},
    [SYS_GET_ROBUST_LIST] = {sys_get_robust_list, SYSCALL_F_NEEDS_PROC},

    /* PSE51 clock and nanosleep (item 15) */
    [SYS_CLOCK_GETTIME] = {sys_clock_gettime, 0},
    [SYS_CLOCK_GETRES] = {sys_clock_getres, 0},
    [SYS_NANOSLEEP] = {sys_nanosleep, 0},

    /* PSE51 memory locking (item 15e) */
    [SYS_MLOCKALL] = {sys_mlockall, 0},
    [SYS_MUNLOCKALL] = {sys_munlockall, 0},

    /* PSE51 synchronization (item 15a) */
    [SYS_MUTEX_INIT] = {sys_mutex_init_h, SYSCALL_F_NEEDS_PROC},
    [SYS_MUTEX_LOCK] = {sys_mutex_lock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_MUTEX_TRYLOCK] = {sys_mutex_trylock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_MUTEX_UNLOCK] = {sys_mutex_unlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_COND_INIT] = {sys_cond_init_h, SYSCALL_F_NEEDS_PROC},
    [SYS_COND_WAIT] = {sys_cond_wait_h, SYSCALL_F_NEEDS_PROC},
    [SYS_COND_TIMEDWAIT] = {sys_cond_timedwait_h, SYSCALL_F_NEEDS_PROC},
    [SYS_COND_SIGNAL] = {sys_cond_signal_h, SYSCALL_F_NEEDS_PROC},
    [SYS_COND_BROADCAST] = {sys_cond_broadcast_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SEM_INIT] = {sys_sem_init_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SEM_WAIT] = {sys_sem_wait_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SEM_TRYWAIT] = {sys_sem_trywait_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SEM_POST] = {sys_sem_post_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SEM_TIMEDWAIT] = {sys_sem_timedwait_h, SYSCALL_F_NEEDS_PROC},

    /* POSIX barriers (item 15i) */
    [SYS_BARRIER_INIT] = {sys_barrier_init_h, SYSCALL_F_NEEDS_PROC},
    [SYS_BARRIER_WAIT] = {sys_barrier_wait_h, SYSCALL_F_NEEDS_PROC},
    [SYS_BARRIER_DESTROY] = {sys_barrier_destroy_h, SYSCALL_F_NEEDS_PROC},

    /* POSIX rwlocks (item 15j) */
    [SYS_RWLOCK_INIT] = {sys_rwlock_init_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_RDLOCK] = {sys_rwlock_rdlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_WRLOCK] = {sys_rwlock_wrlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_TRYRDLOCK] = {sys_rwlock_tryrdlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_TRYWRLOCK] = {sys_rwlock_trywrlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_UNLOCK] = {sys_rwlock_unlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_TIMEDRDLOCK] = {sys_rwlock_timedrdlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_TIMEDWRLOCK] = {sys_rwlock_timedwrlock_h, SYSCALL_F_NEEDS_PROC},
    [SYS_RWLOCK_DESTROY] = {sys_rwlock_destroy_h, SYSCALL_F_NEEDS_PROC},

    /* POSIX message queues (item 15b) */
    [SYS_MQ_OPEN] = {sys_mq_open, SYSCALL_F_NEEDS_PROC},
    [SYS_MQ_CLOSE] = {sys_mq_close, SYSCALL_F_NEEDS_PROC},
    [SYS_MQ_SEND] = {sys_mq_send, SYSCALL_F_NEEDS_PROC},
    [SYS_MQ_RECEIVE] = {sys_mq_receive, SYSCALL_F_NEEDS_PROC},
    [SYS_MQ_TIMEDRECEIVE] = {sys_mq_timedreceive, SYSCALL_F_NEEDS_PROC},

    /* PSE51 scheduling (item 17) */
    [SYS_SCHED_GET_PRIORITY_MIN] = {sys_sched_get_priority_min, 0},
    [SYS_SCHED_GET_PRIORITY_MAX] = {sys_sched_get_priority_max, 0},
    [SYS_SCHED_YIELD] = {sys_sched_yield_pse51, 0},
    [SYS_SCHED_SETPARAM] = {sys_sched_setparam, 0},
    [SYS_SCHED_GETPARAM] = {sys_sched_getparam, 0},

    /* Signals (item 16) */
    [SYS_KILL] = {sys_kill_h, 0},
    [SYS_SIGACTION] = {sys_sigaction_h, SYSCALL_F_NEEDS_PROC},
    [SYS_SIGRETURN] = {sys_sigreturn_h, SYSCALL_F_NEEDS_PROC},

    /* Thread management (item 15d) */
    [SYS_THREAD_CREATE] = {sys_thread_create_h, SYSCALL_F_NEEDS_PROC},
    [SYS_THREAD_JOIN] = {sys_thread_join_h, SYSCALL_F_NEEDS_PROC},
    [SYS_THREAD_DETACH] = {sys_thread_detach_h, SYSCALL_F_NEEDS_PROC},
    [SYS_THREAD_EXIT] = {sys_thread_exit_h, SYSCALL_F_NEEDS_PROC},
    [SYS_THREAD_SELF] = {sys_thread_self_h, 0},

    /* Interval timers (item 15f) */
    [SYS_TIMER_CREATE] = {sys_timer_create_h, SYSCALL_F_NEEDS_PROC},
    [SYS_TIMER_SETTIME] = {sys_timer_settime_h, SYSCALL_F_NEEDS_PROC},
    [SYS_TIMER_DELETE] = {sys_timer_delete_h, SYSCALL_F_NEEDS_PROC},
    [SYS_TIMER_GETTIME] = {sys_timer_gettime_h, SYSCALL_F_NEEDS_PROC},
    [SYS_TIMER_GETOVERRUN] = {sys_timer_getoverrun_h, SYSCALL_F_NEEDS_PROC},
};

/* Security counters, global and irq-safe via atomics. */
static u64 sec_nr_denied;
static u64 sec_nr_enosys;

struct syscall_security_stats syscall_security_stats_get(void)
{
    return (struct syscall_security_stats) {
        .nr_denied = __atomic_load_n(&sec_nr_denied, __ATOMIC_RELAXED),
        .nr_enosys = __atomic_load_n(&sec_nr_enosys, __ATOMIC_RELAXED),
    };
}

/* Centralized authorization gate.
 *
 * Checks run before any handler is invoked:
 * 1. Invalid syscall number -> ENOSYS
 * 2. SYSCALL_F_NEEDS_PROC with no process -> EPERM
 * 3. Per-process syscall_allow bitmask (non-zero = whitelist) -> EACCES
 *
 * Denied syscalls are logged to klog and counted for /api/stats.
 */
i64 syscall_dispatch(struct trap_frame *tf, struct sched_task *td)
{
    u64 nr = tf->a7;
    struct proc *proc = td ? td->proc : NULL;
    u16 tid = td ? td->id : 0;

    /* Gate 1: valid syscall number. */
    if (nr >= SYS_NR || !syscall_table[nr].handler) {
        __atomic_add_fetch(&sec_nr_enosys, 1, __ATOMIC_RELAXED);
        klog_security_event("SEC_DENY", proc ? proc->pid : 0, tid, nr, ENOSYS);
        return -(i64) ENOSYS;
    }

    const struct syscall_entry *ent = &syscall_table[nr];

    /* Gate 2: syscalls requiring a process context. */
    if ((ent->flags & SYSCALL_F_NEEDS_PROC) && !proc) {
        __atomic_add_fetch(&sec_nr_denied, 1, __ATOMIC_RELAXED);
        klog_security_event("SEC_DENY", 0, tid, nr, EPERM);
        return -(i64) EPERM;
    }

    /* Gate 3: per-process syscall allow-list (0 = unrestricted).
     * Two u64 words cover syscall numbers 0..127.
     */
    if (proc && (proc->syscall_allow[0] | proc->syscall_allow[1]) != 0) {
        u32 word = (u32) (nr / 64);
        u64 bit = (u64) 1 << (nr % 64);
        if (!(proc->syscall_allow[word] & bit)) {
            __atomic_add_fetch(&sec_nr_denied, 1, __ATOMIC_RELAXED);
            klog_security_event("SEC_DENY", proc->pid, tid, nr, EACCES);
            return -(i64) EACCES;
        }
    }

#ifdef CONFIG_EVENTLOG_SYSCALLS
    u32 _sc_cpu = get_cpuid();
    KTRACE("event=syscall_entry cpu=%hu tid=%hu nr=%lu", _sc_cpu, (u32) tid,
           (u64) nr);
#endif
    i64 ret = ent->handler(tf, td);

#ifdef CONFIG_EVENTLOG_SYSCALLS
    KTRACE("event=syscall_exit cpu=%hu tid=%hu nr=%lu ret=%lu", _sc_cpu,
           (u32) tid, (u64) nr, (u64) ret);
#endif
    return ret;
}

#include __INC_TEST(syscall)
#include __INC_TEST(clock)
#include __INC_TEST(pse51)
