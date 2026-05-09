/* SPDX-License-Identifier: MIT */
#ifndef MAZU_OPTION_H
#define MAZU_OPTION_H

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/byte.h>

/* NOTE: 'struct_option(name, type)' can only be used in contexts, where
 * 'assert' is available. This is essentially everywhere, except in the
 * functions used to implement 'assert'.
 */

#define struct_option(name, type)                                      \
    struct option_##name {                                             \
        bool is_none;                                                  \
        type unchecked_option_value;                                   \
    };                                                                 \
                                                                       \
    static inline type option_##name##_checked(struct option_##name o) \
    {                                                                  \
        assert(!o.is_none);                                            \
        return o.unchecked_option_value;                               \
    }                                                                  \
                                                                       \
    static inline struct option_##name option_##name##_none(void)      \
    {                                                                  \
        return (struct option_##name) {                                \
            .is_none = true,                                           \
        };                                                             \
    }                                                                  \
                                                                       \
    static inline struct option_##name option_##name##_ok(type t)      \
    {                                                                  \
        struct option_##name o = {                                     \
            .is_none = false,                                          \
            .unchecked_option_value = t,                               \
        };                                                             \
        return o;                                                      \
    }                                                                  \
                                                                       \
    typedef char REQUIRE_SEMICOLON_AFTER_MACRO_STRUCT_OPTION_##name

struct_option(sz, sz);
struct_option(byte_array, struct byte_array);
struct_option(u16, u16);

#endif /* MAZU_OPTION_H */
