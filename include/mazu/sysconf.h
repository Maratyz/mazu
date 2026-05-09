/* SPDX-License-Identifier: MIT */
/* POSIX sysconf() names and feature-test macros for Mazu.
 *
 * Only features actually implemented by Mazu are advertised.
 * Unimplemented _SC_ names return -EINVAL from sys_sysconf().
 */

#ifndef MAZU_SYSCONF_H
#define MAZU_SYSCONF_H

#include <mazu/base.h>

/* Feature-test macros: only defined for implemented POSIX features.
 * Value 200809L indicates POSIX.1-2008 conformance for that feature.
 */
#define _POSIX_SPAWN 200809L
#define _POSIX_PIPE_BUF 512 /* POSIX-mandated minimum */

/* sysconf() name constants.  Subset of POSIX _SC_ names relevant to Mazu. */
#define _SC_PAGE_SIZE 0
#define _SC_PAGESIZE _SC_PAGE_SIZE /* alias */
#define _SC_OPEN_MAX 1
#define _SC_NPROCESSORS_CONF 2
#define _SC_NPROCESSORS_ONLN 3
#define _SC_PIPE_BUF 4
#define _SC_CHILD_MAX 5
#define _SC_MEMLOCK 6

#define _SC_NR 7 /* total number of sysconf names */

/* Kernel-callable sysconf query.  Returns the value for the given _SC_
 * name, or -EINVAL for unknown names.
 */
i64 sys_sysconf_query(i64 name);

#endif /* MAZU_SYSCONF_H */
