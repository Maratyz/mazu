/* SPDX-License-Identifier: MIT */
/* Structured event log (KTRACE) - kernel support file.
 *
 * The KTRACE macro is header-only (include/mazu/eventlog.h).
 * This file exists solely to host the selftest.
 */

#include <mazu/eventlog.h>
#include <mazu/selftest.h>

#include __INC_TEST(eventlog)
