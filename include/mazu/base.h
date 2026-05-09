/* SPDX-License-Identifier: MIT */
#ifndef MAZU_BASE_H
#define MAZU_BASE_H

#ifndef __GNUC__
#error "We use GCC here, go away with your strange, non-GCC compiler"
#endif

/* Base types */

/* Relies on GCC predefined macros:
 * https://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
 */

typedef unsigned char byte;
typedef __UINT8_TYPE__ u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;
typedef __INT8_TYPE__ i8;
typedef __INT16_TYPE__ i16;
typedef __INT32_TYPE__ i32;
typedef __INT64_TYPE__ i64;
/* Only signed pointer and size types are used */
typedef __INTPTR_TYPE__ ptr;
typedef __UINTPTR_TYPE__ uptr;
typedef __PTRDIFF_TYPE__ sz;
typedef __SIZE_TYPE__ usz;
typedef u8 bool;

/* Limits */

#define U8_MAX __UINT8_MAX__
#define U16_MAX __UINT16_MAX__
#define U32_MAX __UINT32_MAX__
#define U64_MAX __UINT64_MAX__
#define I32_MAX __INT32_MAX__
#define I64_MAX __INT64_MAX__
#define UPTR_MAX __UINTPTR_MAX__
#define SZ_MAX __PTRDIFF_MAX__
#define USZ_MAX __SIZE_MAX__

#define BYTE_WIDTH 8
#define U32_WIDTH 32
#define SZ_WIDTH __PTRDIFF_WIDTH__

/* Fundamental macros */

/* Make sure to enable -Wno-keyword-macro so this doesn't annoy you */
#define sizeof(x) ((sz) sizeof(x))
#define countof(arr) (sizeof(arr) / sizeof((arr)[0]))
#define lengthof(str) (countof(str) - 1)
#define alignof(x) ((sz) __alignof__(x))
#define offsetof(type, member) __builtin_offsetof(type, member)
#define __container_of(x, type, member) \
    ((type *) ((const volatile byte *) (x) - offsetof(type, member)))
#define NULL ((void *) 0)
#define true 1
#define false 0
#define STRINGIFY(x) #x
#define CONCAT(a, b) a##b
#define TOSTRING(x) STRINGIFY(x)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BIT(n) (1LLU << (n))
#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(v, a) ((v) & ~((a) - 1))
#define unlikely(x) (__builtin_expect((!!(x)), 0))
#define likely(x) (__builtin_expect((!!(x)), 1))
#define IS_ALIGNED(v, a) (ALIGN_DOWN(v, a) == (v))
#define static_assert(expr, msg) __extension__ _Static_assert(expr, msg)
#if defined(__has_builtin)
#define HAS_BUILTIN(x) __has_builtin(x)
#else
#define HAS_BUILTIN(x) 0
#endif

#if defined(__has_attribute)
#define HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#define HAS_ATTRIBUTE(x) 0
#endif

#if HAS_BUILTIN(__builtin_add_overflow_p)
#define ADD_OVERFLOW(a, b) \
    __builtin_add_overflow_p(a, b, (__typeof__((a) + (b))) 0)
#else
#define ADD_OVERFLOW(a, b)                                               \
    __extension__({                                                      \
        __typeof__((a) + (b)) _sum;                                      \
        __builtin_add_overflow((__typeof__(a)) (a), (__typeof__(b)) (b), \
                               &_sum);                                   \
    })
#endif

#if HAS_BUILTIN(__builtin_sub_overflow_p)
#define SUB_OVERFLOW(a, b) \
    __builtin_sub_overflow_p(a, b, (__typeof__((a) - (b))) 0)
#else
#define SUB_OVERFLOW(a, b)                                               \
    __extension__({                                                      \
        __typeof__((a) - (b)) _diff;                                     \
        __builtin_sub_overflow((__typeof__(a)) (a), (__typeof__(b)) (b), \
                               &_diff);                                  \
    })
#endif

#if HAS_BUILTIN(__builtin_mul_overflow_p)
#define MUL_OVERFLOW(a, b) \
    __builtin_mul_overflow_p(a, b, (__typeof__((a) * (b))) 0)
#else
#define MUL_OVERFLOW(a, b)                                               \
    __extension__({                                                      \
        __typeof__((a) * (b)) _prod;                                     \
        __builtin_mul_overflow((__typeof__(a)) (a), (__typeof__(b)) (b), \
                               &_prod);                                  \
    })
#endif
#define IN_RANGE(a, base, len) \
    ((u64) (a) >= (u64) (base) && ((u64) (a) - (u64) (base)) < (u64) (len))

/* Variadic functions */

#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v) __builtin_va_end(v)
#define va_arg(v, l) __builtin_va_arg(v, l)
#define va_copy(d, s) __builtin_va_copy(d, s)
typedef __builtin_va_list va_list;

/* Attributes */

#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __unused __attribute__((unused))
#define __used __attribute__((used))
#define __section(s) __attribute__((section(s)))
#define __noreturn __attribute__((noreturn))
#define __naked __attribute__((naked))
#define __must_check __attribute__((warn_unused_result))

#if HAS_ATTRIBUTE(fallthrough) || (defined(__GNUC__) && __GNUC__ >= 7)
#define __fallthrough __attribute__((fallthrough))
#else
#define __fallthrough \
    do {              \
    } while (0)
#endif

/* Test infrastructure */

/* Include a test file: #include __INC_TEST(vfs) expands to <tests/tests-vfs.c>.
 * Two-level expansion ensures macro arguments substitute before header-name
 * formation, following L4's __INC_ARCH() pattern.
 */
#define __INC_TEST2(x) <tests/tests-x.c>
#define __INC_TEST(x) __INC_TEST2(x)

#endif /* MAZU_BASE_H */
