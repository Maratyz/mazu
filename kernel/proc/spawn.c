/* SPDX-License-Identifier: MIT */
/* posix_spawn file actions and spawn attributes.
 *
 * Executes file actions (open/close/dup2) in the child's FD table before
 * the child is enqueued.  All mutations hold the child's fd_lock.
 * The child is in EMBRYO state during this phase - not yet visible to
 * other tasks or harts.
 */

#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/spawn.h>
#include <mazu/string.h>
#include <mazu/vfs.h>

#include "pipe.h"

/* Open a file and install it at a specific FD in the child's table.
 * Caller must hold child->fd_lock.
 */
static i32 fa_open_locked(struct proc *child, i32 fd, char *kpath, sz pathlen)
{
    if (fd < 0 || fd >= PROC_FD_MAX)
        return -(i32) EBADF;

    struct str path = str_new(kpath, pathlen);
    struct result_vfs_file fres = vfs_open(path);
    if (fres.is_error)
        return -(i32) fres.code;

    /* Close existing FD if occupied. */
    if (child->fd_table[fd].is_open)
        proc_close_fd_locked(child, fd);

    child->fd_table[fd].is_open = true;
    child->fd_table[fd].is_seekable = true;
    child->fd_table[fd].is_dup = false;
    child->fd_table[fd].offset = 0;
    child->fd_table[fd].file = result_vfs_file_checked(fres);
    return 0;
}

/* Close a specific FD in the child's table.
 * Caller must hold child->fd_lock.
 */
static i32 fa_close_locked(struct proc *child, i32 fd)
{
    if (fd < 0 || fd >= PROC_FD_MAX)
        return -(i32) EBADF;

    if (child->fd_table[fd].is_open)
        proc_close_fd_locked(child, fd);
    return 0;
}

/* Duplicate oldfd to newfd in the child's table.
 * Caller must hold child->fd_lock.
 */
static i32 fa_dup2_locked(struct proc *child, i32 oldfd, i32 newfd)
{
    if (oldfd < 0 || oldfd >= PROC_FD_MAX)
        return -(i32) EBADF;
    if (newfd < 0 || newfd >= PROC_FD_MAX)
        return -(i32) EBADF;
    if (!child->fd_table[oldfd].is_open)
        return -(i32) EBADF;

    if (oldfd == newfd)
        return 0;

    if (child->fd_table[newfd].is_open)
        proc_close_fd_locked(child, newfd);

    child->fd_table[newfd] = child->fd_table[oldfd];
    child->fd_table[newfd].is_dup = true;

    if (child->fd_table[newfd].is_pipe) {
        u64 pf = spin_lock_irqsave(&child->fd_table[newfd].pipe->lock);
        if (child->fd_table[newfd].pipe_read_end)
            child->fd_table[newfd].pipe->readers++;
        else
            child->fd_table[newfd].pipe->writers++;
        spin_unlock_irqrestore(&child->fd_table[newfd].pipe->lock, pf);
    }
    return 0;
}

i32 spawn_apply_file_actions(struct proc *child,
                             const struct spawn_file_action *actions,
                             sz count)
{
    if (count == 0)
        return 0;
    if (count > SPAWN_FA_MAX)
        return -(i32) EINVAL;

    i32 rc = 0;
    u64 fd_flags = proc_fd_lock_irqsave(child);

    for (sz i = 0; i < count; i++) {
        const struct spawn_file_action *fa = &actions[i];

        switch (fa->type) {
        case SPAWN_FA_OPEN:
            if (fa->pathlen == 0 || fa->pathlen > SPAWN_FA_PATH_MAX) {
                rc = -(i32) EINVAL;
                goto out;
            }
            rc = fa_open_locked(child, fa->fd, (char *) fa->path, fa->pathlen);
            break;
        case SPAWN_FA_CLOSE:
            rc = fa_close_locked(child, fa->fd);
            break;
        case SPAWN_FA_DUP2:
            rc = fa_dup2_locked(child, fa->fd, fa->newfd);
            break;
        default:
            rc = -(i32) EINVAL;
            goto out;
        }

        if (rc < 0)
            goto out;
    }

out:
    proc_fd_unlock_irqrestore(child, fd_flags);
    return rc;
}

i32 spawn_apply_attr(struct proc *child __unused,
                     const struct spawn_attr *attr,
                     u8 *out_prio)
{
    if (!attr)
        return 0;

    /* Reject unknown flags. */
    if (attr->flags & ~(u32) SPAWN_ATTR_SETPRIO)
        return -(i32) EINVAL;

    if (attr->flags & SPAWN_ATTR_SETPRIO) {
        if (attr->sched_priority >= CONFIG_SCHED_NPRIO)
            return -(i32) EINVAL;
        *out_prio = attr->sched_priority;
    }

    return 0;
}

#include __INC_TEST(spawn)
