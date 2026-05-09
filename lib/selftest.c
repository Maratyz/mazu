/* SPDX-License-Identifier: MIT */
/* Self-test runner.
 *
 * Iterates DEFINE_SELFTEST entries collected by the linker into
 * .selftest, runs each, and prints OK/FAIL + test name.
 * Returns 0 if all pass, 1 if any fail.
 */

#if CONFIG_SEMIHOSTING

#include <mazu/base.h>
#include <mazu/errordef.h>
#include <mazu/print.h>
#include <mazu/selftest.h>
#include <mazu/string.h>
#include <semihost.h>

extern struct selftest __selftest_start[];
extern struct selftest __selftest_end[];

/* Print one aligned progress line.
 * Format: "  [NNN/TTT] <name>.... "
 */
static void print_test_line(const char *name,
                            sz name_len,
                            i32 index,
                            i32 total,
                            sz width)
{
    char buf[32];
    struct str_buf sb = str_buf_new(buf, 0, sizeof(buf));

    /* "[NNN/TTT] " prefix */
    print_fmt(sb, STR("  [%03ld/%03ld] "), (i64) index, (i64) total);
    print_str(str_new((char *) name, name_len));

    /* Pad with dots to 'width' columns after the name. */
    sz pad = (name_len < width) ? width - name_len : 1;
    for (sz i = 0; i < pad; i++)
        print_str(STR("."));
    print_str(STR(" "));
}

i32 run_selftests(void)
{
    i32 n_pass = 0;
    i32 n_fail = 0;
    i32 n_total = (i32) (__selftest_end - __selftest_start);
    i32 index = 0;

    char buf[80];
    struct str_buf sb = str_buf_new(buf, 0, sizeof(buf));
    print_fmt(sb, STR("Running %ld selftests:\n\n"), (i64) n_total);

    for (struct selftest *t = __selftest_start; t < __selftest_end; t++) {
        index++;
        const char *p = t->name;
        sz name_len = 0;
        while (p[name_len] != '\0')
            name_len++;

        print_test_line(p, name_len, index, n_total, 40);

        i32 rc = t->test_fn();
        if (rc == 0) {
            n_pass++;
            print_str(STR("[ OK ]\n"));
        } else {
            n_fail++;
            sb = str_buf_new(buf, 0, sizeof(buf));
            print_fmt(sb, STR("[FAIL] (rc=%ld)\n"), (i64) rc);
        }
    }

    /* Summary line. */
    sb = str_buf_new(buf, 0, sizeof(buf));
    print_str(STR("\n"));
    if (n_fail == 0) {
        print_fmt(sb, STR("OK: all %ld tests passed\n"), (i64) n_pass);
    } else {
        print_fmt(sb, STR("FAILED: %ld passed, %ld FAILED (out of %ld)\n"),
                  (i64) n_pass, (i64) n_fail, (i64) n_total);
    }

    return n_fail > 0 ? 1 : 0;
}

void selftest_check_and_run(void)
{
    char cmdline[128];
    sz cmdline_len = 0;
    if (semihost_get_cmdline(cmdline, sizeof(cmdline) - 1, &cmdline_len) != 0)
        return;
    if (cmdline_len >= (sz) sizeof(cmdline))
        cmdline_len = (sz) sizeof(cmdline) - 1;
    cmdline[cmdline_len] = '\0';

    /* Check for "selftest" as a space-delimited token. */
    struct str cmdline_str = str_new(cmdline, cmdline_len);
    struct str target = STR("selftest");
    struct option_sz pos = str_find_substring(cmdline_str, target);
    if (pos.is_none)
        return;
    sz p = option_sz_checked(pos);
    bool at_start = (p == 0 || cmdline[p - 1] == ' ');
    bool at_end =
        (p + target.len >= cmdline_len || cmdline[p + target.len] == ' ');
    if (!(at_start && at_end))
        return;

    print_str(STR("=== Mazu self-test (semihosting) ===\n\n"));
    i32 rc = run_selftests();
    semihost_exit(rc);
}

#include __INC_TEST(selftest)

#endif /* CONFIG_SEMIHOSTING */
