/* SPDX-License-Identifier: MIT */
/* Core string-view types.
 *
 * Mazu uses explicit pointer plus length strings instead of NUL-terminated
 * C strings at subsystem boundaries. That keeps formatting, parsing, VFS,
 * networking, and logging on one representation with no hidden strlen()
 * cost and no requirement that buffers be NUL-terminated.
 *
 * This header is split from string.h so low-level code such as assert and
 * printk can use struct str without pulling in higher-level helpers that
 * themselves depend on assert.
 */

#ifndef MAZU_STRINGDEF_H
#define MAZU_STRINGDEF_H

#include <mazu/base.h>

struct str {
    char *dat;
    sz len;
};

struct str_buf {
    char *dat;
    sz len;
    sz cap;
};

/* STR() is the literal bridge into the string model.
 * It turns a compile-time string literal or fixed char array into struct str
 * with zero runtime work, which is why it appears throughout logging, command
 * parsing, protocol handling, and path manipulation code.
 */
#define STR(s)                         \
    (struct str)                       \
    {                                  \
        .dat = (s), .len = lengthof(s) \
    }

/* STR_STATIC() serves the same purpose as STR() for static initializers.
 * Use it when a context requires a constant initializer rather than a
 * compound literal expression.
 */
#define STR_STATIC(s) {.dat = (s), .len = lengthof(s)}

static inline struct str_buf str_buf_new(char *dat, sz len, sz cap)
{
    return (struct str_buf) {.dat = dat, .len = len, .cap = cap};
}

static inline void str_buf_clear(struct str_buf *buf)
{
    if (buf)
        buf->len = 0;
}

static inline struct str str_from_buf(struct str_buf buf)
{
    return (struct str) {.dat = buf.dat, .len = buf.len};
}

static inline struct str str_from_range(char *beg, char *end)
{
    return (struct str) {.dat = beg, .len = end - beg};
}

static inline struct str str_new(char *dat, sz len)
{
    return (struct str) {.dat = dat, .len = len};
}

#endif /* MAZU_STRINGDEF_H */
