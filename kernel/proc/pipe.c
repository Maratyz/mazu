/* SPDX-License-Identifier: MIT */
/* Kernel pipe implementation.
 *
 * Fixed-size (4 KiB) circular buffer with blocking read/write.
 * Read blocks when buffer empty and writers exist; returns 0 (EOF) when
 * no writers remain.  Write blocks when buffer full and readers exist;
 * returns -EPIPE when no readers remain.
 *
 * The buffer uses head/tail indices with power-of-2 masking.  Space
 * used = tail - head; space free = PIPE_BUF_SIZE - used.
 *
 * Pipe lifetime: allocated by sys_pipe, freed when the last close
 * brings both readers and writers to zero.
 */

#include <mazu/error.h>
#include <mazu/kvalloc.h>
#include <mazu/proc.h>
#include <mazu/string.h>

#include "pipe.h"

static inline sz pipe_used(struct pipe *p)
{
    return p->tail - p->head;
}

static inline sz pipe_space(struct pipe *p)
{
    return PIPE_BUF_SIZE - pipe_used(p);
}

static void pipe_free(struct pipe *p)
{
    kvalloc_free(byte_array_new((byte *) p, sizeof(struct pipe)));
}

static void pipe_close_end(struct pipe *p, bool close_reader)
{
    u64 flags = spin_lock_irqsave(&p->lock);
    if (close_reader)
        p->readers--;
    else
        p->writers--;

    bool free_pipe = (p->readers == 0 && p->writers == 0);
    /* Wake the peer side only when this endpoint is the last of its kind but
     * the opposite side still has live references.  When free_pipe is true
     * both counts are zero, so no task can be blocked on either waitqueue
     * (a blocked reader/writer holds an fd that keeps its count > 0) and
     * waking would touch the struct after the other hart frees it.
     */
    bool wake_peer =
        !free_pipe && (close_reader ? (p->readers == 0) : (p->writers == 0));
    spin_unlock_irqrestore(&p->lock, flags);

    if (wake_peer) {
        if (close_reader)
            wake_up(&p->wr_wq, 128);
        else
            wake_up(&p->rd_wq, 128);
    }

    if (free_pipe)
        pipe_free(p);
}

struct pipe *pipe_alloc(void)
{
    struct option_byte_array ba = kvalloc_alloc(sizeof(struct pipe), 8);
    if (ba.is_none)
        return NULL;
    struct byte_array bytes = option_byte_array_checked(ba);
    struct pipe *p = (struct pipe *) bytes.dat;

    memset(p, 0, sizeof(*p));
    p->lock = (spinlock_t) SPINLOCK_INITIALIZER;
    p->readers = 1;
    p->writers = 1;
    init_waitqueue_head(&p->rd_wq);
    init_waitqueue_head(&p->wr_wq);
    return p;
}

i64 pipe_read(struct pipe *p, void *kbuf, sz len)
{
    if (len == 0)
        return 0;

    for (;;) {
        /* Block until data available or all writers closed. */
        wait_event(p->rd_wq, pipe_used(p) > 0 || p->writers == 0);

        u64 flags = spin_lock_irqsave(&p->lock);
        sz used = pipe_used(p);
        if (used > 0) {
            sz n = (len < used) ? len : used;
            for (sz i = 0; i < n; i++)
                ((u8 *) kbuf)[i] = p->buf[(p->head + i) & (PIPE_BUF_SIZE - 1)];
            p->head += n;
            spin_unlock_irqrestore(&p->lock, flags);
            wake_up(&p->wr_wq, 1);
            return (i64) n;
        }
        /* Empty: check for EOF. */
        bool eof = (p->writers == 0);
        spin_unlock_irqrestore(&p->lock, flags);
        if (eof)
            return 0;
        /* Spurious wakeup with concurrent reader: retry. */
    }
}

i64 pipe_write(struct pipe *p, const void *kbuf, sz len)
{
    if (len == 0)
        return 0;

    /* POSIX: writes <= PIPE_BUF are atomic - the entire message must
     * appear contiguously in the pipe, never interleaved with another
     * writer's data.  Since sys_write caps pipe writes to PIPE_BUF_SIZE,
     * every call here is in the atomicity range.  Block until the
     * full len bytes of space are available, then write in one shot
     * under the lock.
     */
    for (;;) {
        wait_event(p->wr_wq, pipe_space(p) >= len || p->readers == 0);

        u64 flags = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -(i64) EPIPE;
        }
        if (pipe_space(p) >= len) {
            for (sz i = 0; i < len; i++)
                p->buf[(p->tail + i) & (PIPE_BUF_SIZE - 1)] =
                    ((const u8 *) kbuf)[i];
            p->tail += len;
            spin_unlock_irqrestore(&p->lock, flags);
            wake_up(&p->rd_wq, 1);
            return (i64) len;
        }
        spin_unlock_irqrestore(&p->lock, flags);
        /* Concurrent writer consumed space between wait_event and lock
         * acquisition - retry.
         */
    }
}

void pipe_close_read(struct pipe *p)
{
    pipe_close_end(p, true);
}

void pipe_close_write(struct pipe *p)
{
    pipe_close_end(p, false);
}

#include __INC_TEST(pipe)
