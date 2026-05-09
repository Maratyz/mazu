/* SPDX-License-Identifier: MIT */
/* Plan 9-style /dev synthetic filesystem.
 *
 * Provides device files as VFS nodes:
 *   /dev/null      discard sink (writes succeed, reads return EOF)
 *   /dev/zero      infinite zero source
 *   /dev/console   UART read/write (write-only for now)
 *   /dev/time      nanosecond timestamp (read-only)
 *   /dev/sysname   system name "mazu" (read-only)
 *   /dev/osversion git commit hash (read-only)
 */

#ifndef MAZU_DEVFS_H
#define MAZU_DEVFS_H

#include <mazu/vfs.h>

/* Return the VFS ops vtable for the devfs backend.  ctx is unused (NULL). */
struct vfs_ops devfs_vfs_ops(void);

#endif /* MAZU_DEVFS_H */
