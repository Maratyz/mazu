/* SPDX-License-Identifier: MIT */
/* Kernel pipe: fixed-size circular buffer with blocking I/O.
 *
 * Each pipe has a read end and a write end, tracked by per-end
 * refcounts (readers/writers).  Blocking uses embedded wait queues.
 * Lock ordering: fd_lock first, then pipe->lock (never reversed).
 * pipe->lock -> wake_up(wq) is safe (wq->lock is not lockdep-tracked).
 */

#ifndef MAZU_PIPE_H
#define MAZU_PIPE_H

#include <mazu/base.h>
#include <mazu/spinlock.h>

#include "../sched/waitqueue.h"

#define PIPE_BUF_SIZE 4096 /* must be power-of-2 */

struct pipe {
    spinlock_t lock;
    u32 readers;
    u32 writers;
    sz head;                      /* read position (advances on read) */
    sz tail;                      /* write position (advances on write) */
    struct wait_queue_head rd_wq; /* readers block here */
    struct wait_queue_head wr_wq; /* writers block here */
    u8 buf[PIPE_BUF_SIZE];
};

/* Allocate a new pipe with readers=1, writers=1. */
struct pipe *pipe_alloc(void);

/* Read up to len bytes from pipe into kbuf (kernel buffer).
 * Blocks if pipe is empty and writers exist.
 * Returns bytes read, 0 on EOF (no writers), or negative errno.
 */
i64 pipe_read(struct pipe *p, void *kbuf, sz len);

/* Write len bytes from kbuf (kernel buffer) into pipe.
 * Blocks until all bytes are written (POSIX atomic for len <= PIPE_BUF_SIZE).
 * Returns len on success, or -EPIPE if no readers remain.
 */
i64 pipe_write(struct pipe *p, const void *kbuf, sz len);

/* Close one end of the pipe.  Decrements the refcount and wakes
 * blocked waiters on the opposite end.  Frees the pipe when both
 * readers and writers reach zero.
 */
void pipe_close_read(struct pipe *p);
void pipe_close_write(struct pipe *p);

#endif /* MAZU_PIPE_H */
