/* SPDX-License-Identifier: MIT */
#ifndef MAZU_BUDDY_H
#define MAZU_BUDDY_H

#include <mazu/alloc.h>
#include <mazu/arena.h>
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/list.h>
#include <mazu/option.h>

#define PAGE_SIZE_SHIFT 12
#define N_FREE_LISTS ((sizeof(void *) * BYTE_WIDTH) - PAGE_SIZE_SHIFT)

struct block {
    struct list_head link;
    sz ord;
};

struct buddy {
    struct block avail[N_FREE_LISTS];
    struct byte_array bitmap;
    sz max_ord;
    byte *base;
};

/* Initialize a buddy allocator. 'arn' is used to allocate the buddy allocators
 * structures so the return value will be a pointer from 'arn'.
 */
struct buddy *buddy_init(struct byte_array ba, struct arena *arn);

/* Allocate 'size' bytes from the given buddy allocator. 'buddy' must be
 * non-NULL and 'size' must be greater than zero. The byte array that this
 * function returns will be aligned to a page boundary.
 */
struct option_byte_array buddy_alloc(struct buddy *buddy, sz size);

/* Free an allocation from the given buddy allocator. 'buddy' must be non-NULL,
 * and 'ba.len' must match the size of the original allocation.
 */
void buddy_free(struct buddy *buddy, struct byte_array ba);

struct buddy_stats {
    sz total_pages; /* total pages managed by this allocator */
    sz free_pages;  /* pages currently available (sum across all free lists) */
};

struct buddy_stats buddy_stats_get(struct buddy *buddy);

#endif /* MAZU_BUDDY_H */