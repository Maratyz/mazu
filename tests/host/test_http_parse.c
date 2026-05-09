/* SPDX-License-Identifier: MIT */
/* Host tests for HTTP integer parser (http_parse_u64).
 *
 * Source: user/net/web.c lines 1012-1028 (http_parse_u64).
 * Pure function depending only on struct str and overflow macros from base.h.
 */

#include <mazu/base.h>
#include <mazu/stringdef.h>

/* Copy of http_parse_u64 from user/net/web.c (keep in sync). */
static bool http_parse_u64(struct str s, u64 *out)
{
    if (s.len == 0)
        return false;

    u64 v = 0;
    for (sz i = 0; i < s.len; i++) {
        char c = s.dat[i];
        if (c < '0' || c > '9')
            return false;
        u64 digit = (u64) (c - '0');
        if (MUL_OVERFLOW(v, (u64) 10) || ADD_OVERFLOW(v * 10, digit))
            return false;
        v = v * 10 + digit;
    }
    *out = v;
    return true;
}

/* Test: valid decimal strings. */
int test_parse_u64_valid(void)
{
    u64 val = 0;
    if (!http_parse_u64(STR("12345"), &val))
        return 1;
    if (val != 12345)
        return 1;

    if (!http_parse_u64(STR("1"), &val))
        return 1;
    if (val != 1)
        return 1;

    return 0;
}

/* Test: zero parses correctly. */
int test_parse_u64_zero(void)
{
    u64 val = 99;
    if (!http_parse_u64(STR("0"), &val))
        return 1;
    if (val != 0)
        return 1;
    return 0;
}

/* Test: U64_MAX boundary (18446744073709551615). */
int test_parse_u64_max(void)
{
    u64 val = 0;
    if (!http_parse_u64(STR("18446744073709551615"), &val))
        return 1;
    if (val != U64_MAX)
        return 1;
    return 0;
}

/* Test: empty string rejected. */
int test_parse_u64_empty(void)
{
    u64 val = 0;
    struct str empty = {.dat = (char *) "", .len = 0};
    if (http_parse_u64(empty, &val))
        return 1; /* should return false */
    return 0;
}

/* Test: non-digit character rejected. */
int test_parse_u64_non_digit(void)
{
    u64 val = 0;
    if (http_parse_u64(STR("abc"), &val))
        return 1;
    if (http_parse_u64(STR("12x4"), &val))
        return 1;
    if (http_parse_u64(STR(" 123"), &val))
        return 1;
    if (http_parse_u64(STR("123 "), &val))
        return 1;
    return 0;
}

/* Test: overflow detection (value > U64_MAX). */
int test_parse_u64_overflow(void)
{
    u64 val = 0;
    /* U64_MAX + 1 = 18446744073709551616 */
    if (http_parse_u64(STR("18446744073709551616"), &val))
        return 1;
    /* Obviously huge number. */
    if (http_parse_u64(STR("99999999999999999999"), &val))
        return 1;
    return 0;
}

/* Test: leading alphabetic characters rejected. */
int test_parse_u64_leading_alpha(void)
{
    u64 val = 0;
    if (http_parse_u64(STR("Content-Length"), &val))
        return 1;
    return 0;
}
