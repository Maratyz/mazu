/* SPDX-License-Identifier: MIT */
/* Plan 9-style /proc synthetic filesystem.
 *
 * Provides system information as virtual files:
 *   /proc/meminfo   free/used page counts from the kernel allocator
 *   /proc/uptime    system uptime in seconds and milliseconds
 *   /proc/cpuinfo   per-hart summary (hartid, online status)
 */

#ifndef MAZU_PROCFS_H
#define MAZU_PROCFS_H

#include <mazu/vfs.h>

/* Return the VFS ops vtable for the procfs backend.  ctx is unused (NULL). */
struct vfs_ops procfs_vfs_ops(void);

#endif /* MAZU_PROCFS_H */
