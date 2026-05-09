/* SPDX-License-Identifier: MIT */
#ifndef MAZU_FMT_H
#define MAZU_FMT_H

#include <mazu/base.h>
#include <mazu/errordef.h>
#include <mazu/string.h>

/* Formatting is based on: https://nullprogram.com/blog/2023/02/13/ */

/* All functions in this file work by appending something to the buffer that
 * they're given. 'fmt' and 'vfmt' are wrappers around the 'append_*' functions.
 * 'fmt' and 'vfmt' implement a printf-like syntax for convenience while the
 * 'append_*' functions implement formatting of different data types.
 */

static inline struct result fmt_append_i64(i64 x, struct str_buf *buf)
{
    if (!buf)
        return result_error(EINVAL);

    char tmp[64];
    char *end = tmp + countof(tmp);
    char *beg = end;

    u64 d = x < 0 ? (u64) (-(x + 1)) + 1 : (u64) x;
    do {
        *(--beg) = '0' + (char) (d % 10);
    } while (d /= 10);

    if (x < 0) {
        *(--beg) = '-';
    }

    return str_buf_append(buf, str_from_range(beg, end));
}

static inline struct result fmt_append_u64(u64 x, struct str_buf *buf)
{
    if (!buf)
        return result_error(EINVAL);

    char tmp[64];
    char *end = tmp + countof(tmp);
    char *beg = end;

    do {
        *(--beg) = '0' + (x % 10);
    } while (x /= 10);

    return str_buf_append(buf, str_from_range(beg, end));
}

enum fmt_hex_alpha { HEX_ALPHA_UPPER, HEX_ALPHA_LOWER };

static inline struct result fmt_append_hex(u64 x,
                                           enum fmt_hex_alpha alpha,
                                           struct str_buf *buf)
{
    if (!buf)
        return result_error(EINVAL);

    char tmp[64];
    char *end = tmp + countof(tmp);
    char *beg = end;

    const char *a = NULL;
    switch (alpha) {
    case HEX_ALPHA_LOWER:
        a = "0123456789abcdef";
        break;
    case HEX_ALPHA_UPPER:
        a = "0123456789ABCDEF";
        break;
    }

    do {
        *--beg = a[x & 0xf];
    } while (x >>= 4);

    return str_buf_append(buf, str_from_range(beg, end));
}

static inline struct result fmt_append_ptr(void *p, struct str_buf *buf)
{
    return fmt_append_hex((u64) p, HEX_ALPHA_LOWER, buf);
}

static inline struct result fmt_vfmt(struct str_buf *buf,
                                     struct str fmt,
                                     va_list argp)
{
    enum { NONE, L, HH, H } modifier = NONE;
    enum { NOTHING, MODIFIER, CONVERSION } expect = NOTHING;
    struct result res = result_ok();
    sz i = 0;
    int width = 0;
    bool zero_fill = false;
    bool left_align = false;

    if (!buf || !buf->dat || !fmt.dat || fmt.len < 0)
        return result_error(EINVAL);

    while (i < fmt.len) {
        if (expect == NOTHING && fmt.dat[i] == '%') {
            expect = MODIFIER;
            modifier = NONE;
            width = 0;
            zero_fill = false;
            left_align = false;
            i++;
            continue;
        }

        if (expect == NOTHING) {
            res = str_buf_append_char(buf, fmt.dat[i]);
            if (res.is_error)
                return res;
            i++;
            continue;
        }

        if (expect == MODIFIER) {
            /* Consume '-' flag (left-align). */
            if (fmt.dat[i] == '-') {
                left_align = true;
                i++;
                continue;
            }
            /* Consume '0' flag (zero-fill); only valid before width digits. */
            if (fmt.dat[i] == '0' && width == 0 && !zero_fill) {
                zero_fill = true;
                i++;
                continue;
            }
            /* Consume width digits. */
            if (fmt.dat[i] >= '0' && fmt.dat[i] <= '9') {
                width = width * 10 + (fmt.dat[i] - '0');
                i++;
                continue;
            }

            switch (fmt.dat[i]) {
            case 'l':
                modifier = L;
                if (i + 1 < fmt.len && fmt.dat[i + 1] == 'l')
                    i += 2;
                else
                    i++;
                break;
            case 'h':
                if (i + 1 < fmt.len && fmt.dat[i + 1] == 'h') {
                    modifier = HH;
                    i += 2;
                } else {
                    modifier = H;
                    i++;
                }
                break;
            default:
                break;
            }

            expect = CONVERSION;
            continue;
        }

        if (expect == CONVERSION) {
            /* When a field width is set, format into a staging buffer
             * to measure the result and apply padding.
             */
            char pad_tmp[64];
            struct str_buf pad_buf = str_buf_new(pad_tmp, 0, sizeof(pad_tmp));
            struct str_buf *out = (width > 0) ? &pad_buf : buf;

            /* Branches consume different va_arg types (i64/i32, u64/u32)
             * despite textual similarity.
             */
            switch (fmt.dat[i]) {
            case 'i':
            case 'd':
                if (modifier == L)
                    res = fmt_append_i64((i64) va_arg(argp, i64), out);
                else if (modifier == H)
                    res = fmt_append_i64((i64) (i16) va_arg(argp, i32), out);
                else if (modifier == HH)
                    res = fmt_append_i64((i64) (i8) va_arg(argp, i32), out);
                else
                    res = fmt_append_i64((i64) va_arg(argp, i32), out);
                break;
            case 'u':
                if (modifier == L)
                    res = fmt_append_u64((u64) va_arg(argp, u64), out);
                else if (modifier == H)
                    res = fmt_append_u64((u64) va_arg(argp, u32) & 0xffff, out);
                else if (modifier == HH)
                    res = fmt_append_u64((u64) va_arg(argp, u32) & 0xff, out);
                else
                    res = fmt_append_u64((u64) va_arg(argp, u32), out);
                break;
            case 'x':
                if (modifier == L)
                    res = fmt_append_hex((u64) va_arg(argp, u64),
                                         HEX_ALPHA_LOWER, out);
                else if (modifier == H)
                    res = fmt_append_hex((u64) va_arg(argp, u32) & 0xffff,
                                         HEX_ALPHA_LOWER, out);
                else if (modifier == HH)
                    res = fmt_append_hex((u64) va_arg(argp, u32) & 0xff,
                                         HEX_ALPHA_LOWER, out);
                else
                    res = fmt_append_hex((u64) va_arg(argp, u32),
                                         HEX_ALPHA_LOWER, out);
                break;
            case 'X':
                if (modifier == L)
                    res = fmt_append_hex((u64) va_arg(argp, u64),
                                         HEX_ALPHA_UPPER, out);
                else if (modifier == H)
                    res = fmt_append_hex((u64) va_arg(argp, u32) & 0xffff,
                                         HEX_ALPHA_UPPER, out);
                else if (modifier == HH)
                    res = fmt_append_hex((u64) va_arg(argp, u32) & 0xff,
                                         HEX_ALPHA_UPPER, out);
                else
                    res = fmt_append_hex((u64) va_arg(argp, u32),
                                         HEX_ALPHA_UPPER, out);
                break;
            /* End of va_arg format specifier cases. */
            case 's': {
                struct str s = va_arg(argp, struct str);
                if (s.dat && s.len >= 0)
                    res = str_buf_append(out, s);
                else
                    res = str_buf_append(out, STR("(NULL)"));
                break;
            }
            case 'c':
                res = str_buf_append_char(out, (char) va_arg(argp, i32) & 0xff);
                break;
            case '%':
                res = str_buf_append_char(buf, '%');
                break;
            default:
                res = result_error(EINVAL);
                break;
            }

            if (res.is_error)
                return res;

            /* Emit padding + content (right-align) or content + padding
             * (left-align).
             */
            if (width > 0) {
                struct str formatted = str_from_buf(pad_buf);
                sz pad =
                    (sz) width > formatted.len ? (sz) width - formatted.len : 0;
                if (left_align) {
                    res = str_buf_append(buf, formatted);
                    if (res.is_error)
                        return res;
                    for (sz p = 0; p < pad; p++) {
                        res = str_buf_append_char(buf, ' ');
                        if (res.is_error)
                            return res;
                    }
                } else {
                    char fill = zero_fill ? '0' : ' ';
                    /* When zero-filling a negative number, emit the '-'
                     * before the padding so -3 with %05d becomes "-0003"
                     * rather than "000-3".
                     */
                    sz content_start = 0;
                    if (zero_fill && formatted.len > 0 &&
                        formatted.dat[0] == '-') {
                        res = str_buf_append_char(buf, '-');
                        if (res.is_error)
                            return res;
                        content_start = 1;
                    }
                    for (sz p = 0; p < pad; p++) {
                        res = str_buf_append_char(buf, fill);
                        if (res.is_error)
                            return res;
                    }
                    res = str_buf_append(
                        buf, str_new(formatted.dat + content_start,
                                     formatted.len - content_start));
                    if (res.is_error)
                        return res;
                }
            }

            expect = NOTHING;
            i++;
            continue;
        }
    }

    return res;
}

static inline struct result fmt(struct str_buf *buf, struct str fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);
    struct result res = fmt_vfmt(buf, fmt, argp);
    va_end(argp);
    return res;
}

/* Append "key val\n" where val is a decimal u64.
 * Used by synthetic filesystems (procfs, netfs).
 */
static inline void fmt_append_kv_u64(struct str_buf *sb,
                                     struct str key,
                                     u64 val)
{
    str_buf_append(sb, key);
    str_buf_append_char(sb, ' ');
    fmt_append_u64(val, sb);
    str_buf_append_char(sb, '\n');
}

#endif /* MAZU_FMT_H */
