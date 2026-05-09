/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ERROR_H
#define MAZU_ERROR_H

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/errordef.h>

/* NOTE: 'struct_result(name, type)' can only be used in contexts, where
 * 'assert' is available. This is essentially everywhere, except in the
 * functions
 * used to implement 'assert'.
 */

#define struct_result(name, type)                                      \
    struct result_##name {                                             \
        bool is_error;                                                 \
        u16 code;                                                      \
        type unchecked_result_value;                                   \
    };                                                                 \
                                                                       \
    static inline type result_##name##_checked(struct result_##name r) \
    {                                                                  \
        assert(!r.is_error);                                           \
        return r.unchecked_result_value;                               \
    }                                                                  \
                                                                       \
    static inline struct result_##name result_##name##_error(u16 code) \
    {                                                                  \
        struct result_##name r = {                                     \
            .is_error = true,                                          \
            .code = code,                                              \
        };                                                             \
        return r;                                                      \
    }                                                                  \
                                                                       \
    static inline struct result_##name result_##name##_ok(type t)      \
    {                                                                  \
        struct result_##name r = {                                     \
            .is_error = false,                                         \
            .code = 0,                                                 \
            .unchecked_result_value = t,                               \
        };                                                             \
        return r;                                                      \
    }                                                                  \
                                                                       \
    typedef char REQUIRE_SEMICOLON_AFTER_MACRO_STRUCT_RESULT_##name

struct_result(sz, sz);
struct_result(ptr, ptr);
struct_result(u32, u32);
struct_result(bool, bool);

#endif /* MAZU_ERROR_H */
