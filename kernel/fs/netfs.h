/* SPDX-License-Identifier: MIT */
/* Plan 9-style /net synthetic filesystem.
 *
 * Exposes network state as read-only virtual files:
 *   /net/arp       ARP table entries
 *   /net/iface     network interface info (IP, MAC, MTU)
 *   /net/tcp/stats TCP stack statistics
 */

#ifndef MAZU_NETFS_H
#define MAZU_NETFS_H

#include <mazu/vfs.h>

/* Return the VFS ops vtable for the netfs backend.  ctx is unused (NULL). */
struct vfs_ops netfs_vfs_ops(void);

#endif /* MAZU_NETFS_H */
