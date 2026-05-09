/* SPDX-License-Identifier: MIT */
/* POSIX time types for PSE51 clock/timer syscalls.
 *
 * Minimal definitions matching the POSIX spec.  Only types actually
 * used by Mazu syscalls are defined here.
 */

#ifndef MAZU_POSIX_TIME_H
#define MAZU_POSIX_TIME_H

#include <mazu/base.h>

struct timespec {
    i64 tv_sec;
    i64 tv_nsec;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

#define NSEC_PER_SEC 1000000000LL
#define NSEC_PER_MSEC 1000000LL
#define NSEC_PER_USEC 1000LL

#endif /* MAZU_POSIX_TIME_H */
