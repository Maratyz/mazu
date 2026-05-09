/* SPDX-License-Identifier: MIT */
#ifndef MAZU_KVALLOC_H
#define MAZU_KVALLOC_H

#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/errordef.h>
#include <mazu/option.h>

/* Initialize kvalloc. 'vaddrs' is the range of virtual addresses that kvalloc
 * will manage. All addresses in this range must be accessible.
 */
struct result kvalloc_init(struct byte_array vaddrs);

/* Allocate 'n_bytes' bytes with an alignment of at least 'align' bytes.
 * kvalloc must be initialized before calling this function for the first time.
 * Hard-RT rule: never call from interrupt context (timer, external IRQ, IPI).
 * Syscall/exception context is fine — those handlers may block.
 */
struct option_byte_array kvalloc_alloc(sz n_bytes, sz align);

/* Deallocate the memory in the 'ba'.
 * Hard-RT rule: never call from interrupt context (timer, external IRQ, IPI).
 */
void kvalloc_free(struct byte_array ba);

/* Wrappers for mazu/alloc.h. 'a' isn't used. */
void *kvalloc_alloc_wrapper(void *a, sz size, sz align);
void kvalloc_free_wrapper(void *a, void *ptr, sz size);

struct kvalloc_stats {
    sz total_pages;
    sz free_pages;
};

struct kvalloc_stats kvalloc_stats_get(void);

#endif /* MAZU_KVALLOC_H */
