/* SPDX-License-Identifier: MIT */
/* Host-native test runner for Mazu network stack unit tests.
 *
 * Compiles and runs on the build host (no QEMU).  Tests exercise pure
 * functions extracted from kernel/net/tcp.c and user/net/web.c via
 * the kernel header include chain with host-side stubs for arch primitives.
 *
 * Usage: make -C tests/host   (or: make test-host from top level)
 */

#include <stdio.h>
#include <stdlib.h>

struct test_case {
    const char *name;
    int (*fn)(void);
};

/* Test declarations -- defined in per-module test files. */
extern int test_seq_lt_basic(void);
extern int test_seq_lt_wrap(void);
extern int test_seq_le_equal(void);
extern int test_seq_gt_basic(void);
extern int test_seq_geq_equal(void);
extern int test_seq_wrap_boundary(void);

extern int test_hash_range(void);
extern int test_hash_different_tuples(void);

extern int test_parse_u64_valid(void);
extern int test_parse_u64_zero(void);
extern int test_parse_u64_max(void);
extern int test_parse_u64_empty(void);
extern int test_parse_u64_non_digit(void);
extern int test_parse_u64_overflow(void);
extern int test_parse_u64_leading_alpha(void);

#define TEST(f) \
    {           \
        #f, f   \
    }

static struct test_case tests[] = {
    /* TCP sequence number arithmetic */
    TEST(test_seq_lt_basic),
    TEST(test_seq_lt_wrap),
    TEST(test_seq_le_equal),
    TEST(test_seq_gt_basic),
    TEST(test_seq_geq_equal),
    TEST(test_seq_wrap_boundary),

    /* TCP hash function */
    TEST(test_hash_range),
    TEST(test_hash_different_tuples),

    /* HTTP integer parser */
    TEST(test_parse_u64_valid),
    TEST(test_parse_u64_zero),
    TEST(test_parse_u64_max),
    TEST(test_parse_u64_empty),
    TEST(test_parse_u64_non_digit),
    TEST(test_parse_u64_overflow),
    TEST(test_parse_u64_leading_alpha),
};

int main(void)
{
    int n = (int) (sizeof(tests) / sizeof(tests[0]));
    int pass = 0, fail = 0;

    for (int i = 0; i < n; i++) {
        int rc = tests[i].fn();
        if (rc == 0) {
            printf("  PASS  %s\n", tests[i].name);
            pass++;
        } else {
            printf("  FAIL  %s\n", tests[i].name);
            fail++;
        }
    }

    printf("\n%d passed, %d failed (out of %d)\n", pass, fail, n);
    return fail > 0 ? 1 : 0;
}
