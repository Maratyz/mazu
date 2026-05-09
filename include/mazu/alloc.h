/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ALLOC_H
#define MAZU_ALLOC_H

#include <mazu/assert.h>
#include <mazu/base.h>

typedef void *(*alloc_func_t)(void *a, sz size, sz align);
typedef void (*free_func_t)(void *a, void *ptr, sz size);

struct alloc {
    void *a_ptr; /* allocator state */
    alloc_func_t alloc;
    free_func_t free;
};

static inline struct alloc alloc_new(void *a_ptr,
                                     alloc_func_t alloc,
                                     free_func_t free)
{
    assert(a_ptr);
    assert(alloc);
    assert(free);

    return (struct alloc) {
        .a_ptr = a_ptr,
        .alloc = alloc,
        .free = free,
    };
}

static inline void *alloc_alloc(struct alloc a, sz size, sz align)
{
    return a.alloc(a.a_ptr, size, align);
}

static inline void alloc_free(struct alloc a, void *ptr, sz size)
{
    a.free(a.a_ptr, ptr, size);
}

#endif /* MAZU_ALLOC_H */
