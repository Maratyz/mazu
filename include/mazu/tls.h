/* SPDX-License-Identifier: MIT */
/* Per-thread TLS (thread-local storage) for PSE51 errno and kernel slots.
 *
 * Inspired by LK kernel/include/kernel/thread.h: compile-time TLS slots
 * via enum, errno in tls[TLS_ENTRY_ERRNO], accessed via tls_get()/tls_set().
 *
 * PSE51 requires thread-safe errno: each thread gets its own errno slot
 * in the task struct.  No dynamic allocation, no syscall in the hot path.
 */

#ifndef MAZU_TLS_H
#define MAZU_TLS_H

#include <mazu/base.h>

/* TLS slot indices.  Add new entries before MAX_TLS_ENTRY.
 * Keep the enum small: max 8 slots to avoid bloating sched_task.
 */
enum tls_entry {
    TLS_ENTRY_ERRNO = 0, /* per-thread errno for PSE51 user-space ABI */
    MAX_TLS_ENTRY
};

static_assert(MAX_TLS_ENTRY <= 8, "TLS slots exceed maximum");

#endif /* MAZU_TLS_H */
