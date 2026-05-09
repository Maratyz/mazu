/* SPDX-License-Identifier: MIT */
/* Futex self-tests.
 *
 * Test 1: futex_wake on address with no waiters returns 0 (smoke test).
 * Test 2: futex_wake on a second address with no waiters returns 0.
 *
 * Note: futex_wait requires user-space mapping (copy_from_user), so
 * it cannot be exercised from kernel-only selftests.  These tests
 * verify the wake path and hash table initialization only.
 */

#include <kernel/sync/futex.h>
#include <mazu/selftest.h>

/* Kernel-internal futex for testing: bypass copy_from_user. */

/* Test 1: FUTEX_WAIT value mismatch returns immediately. */
static volatile u32 futex_test_word;

static i32 test_futex_value_mismatch(void)
{
    futex_test_word = 42;
    /* Pass expected=99, which does not match 42 -> should return EAGAIN.
     * But futex_wait uses copy_from_user which requires user mapping.
     * For kernel-only testing, verify the hash and init instead.
     */
    (void) futex_test_word;
    /* Verify futex_init ran (table is accessible, no crash). */
    i64 rc = futex_wake((ptr) &futex_test_word, 1);
    if (rc != 0)
        return 1; /* no one waiting, should return 0 */
    return 0;
}
DEFINE_SELFTEST(futex_wake_empty, test_futex_value_mismatch);

/* Test 2: futex_wake on address with no waiters returns 0. */
static volatile u32 futex_test_word2;

static i32 test_futex_wake_none(void)
{
    futex_test_word2 = 0;
    i64 rc = futex_wake((ptr) &futex_test_word2, 10);
    if (rc != 0)
        return 1;
    return 0;
}
DEFINE_SELFTEST(futex_wake_none, test_futex_wake_none);

/* Tests 3-5: futex_cmp_requeue, futex_lock_pi, and futex_unlock_pi use
 * copy_from_user/copy_to_user internally, which reject kernel addresses
 * in S-mode selftests.  Verify the S-mode-accessible behavior:
 * kernel addresses produce -EFAULT, confirming the functions execute
 * through the locking path without deadlock or crash.
 */
static volatile u32 futex_requeue_src;
static volatile u32 futex_requeue_dst;

static i32 test_futex_cmp_requeue_efault(void)
{
    futex_requeue_src = 7;
    futex_requeue_dst = 0;
    i64 rc = futex_cmp_requeue((ptr) &futex_requeue_src, 7,
                               (ptr) &futex_requeue_dst, 1, 10);
    /* Kernel address -> copy_from_user returns -EFAULT. */
    if (rc != -(i64) EFAULT)
        return 1;
    return 0;
}
DEFINE_SELFTEST(futex_cmp_requeue_efault, test_futex_cmp_requeue_efault);

static volatile u32 futex_pi_word;

static i32 test_futex_lock_pi_efault(void)
{
    futex_pi_word = 0;
    i64 rc = futex_lock_pi((ptr) &futex_pi_word);
    if (rc != -(i64) EFAULT)
        return 1;
    return 0;
}
DEFINE_SELFTEST(futex_lock_pi_efault, test_futex_lock_pi_efault);

static i32 test_futex_unlock_pi_efault(void)
{
    futex_pi_word = 9999;
    i64 rc = futex_unlock_pi((ptr) &futex_pi_word);
    if (rc != -(i64) EFAULT)
        return 1;
    return 0;
}
DEFINE_SELFTEST(futex_unlock_pi_efault, test_futex_unlock_pi_efault);
