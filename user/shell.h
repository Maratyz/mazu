/* SPDX-License-Identifier: MIT */
/* Web shell: session management and command interpreter. */

#ifndef MAZU_SHELL_H
#define MAZU_SHELL_H

#include <mazu/arena.h>
#include <mazu/base.h>
#include <mazu/string.h>

#define SHELL_MAX_SESSIONS 4
#define SHELL_OUT_BUF_SIZE 8192

struct shell_session {
    u64 token;       /* Random 64-bit auth token assigned at creation. */
    u64 write_total; /* Total bytes ever written (the "from" offset). */
    u32 sid;
    bool active;
    char out[SHELL_OUT_BUF_SIZE]; /* Circular output buffer. */
};

/* Initialize the shell module.  Must be called before any other shell_*
 * function.  File operations use VFS (must call vfs_init() first).
 */
void shell_init(void);

/* Return the session with the given 'sid', or create a new one.  When all
 * slots are occupied the oldest session (round-robin) is evicted and reused.
 * Never returns NULL.
 */
struct shell_session *shell_find_or_create(u32 sid);

/* Create a new session with the given 'sid' and a freshly generated random
 * token.  If all slots are occupied, the oldest (round-robin) is evicted.
 * Never returns NULL.
 */
struct shell_session *shell_create(u32 sid);

/* Return the session with the given 'sid', or NULL if it doesn't exist. */
struct shell_session *shell_find(u32 sid);

/* Execute the single-line command 'cmd' in session 'sess', writing any output
 * to the session's circular buffer.  'tmp' is a temporary arena.
 */
void shell_exec(struct shell_session *sess, struct str cmd, struct arena tmp);

#endif /* MAZU_SHELL_H */
