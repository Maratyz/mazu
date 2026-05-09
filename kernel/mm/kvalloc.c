/* SPDX-License-Identifier: MIT */
/* Kernel virtual address allocator (kvalloc).
 *
 * This allocator manages virtual memory for use by the kernel using the buddy
 * system.
 *
 * The kvalloc implements a typical alloc/free interface. Kernel subsystems can
 * get memory for their internal structures here. It's recommended that these
 * subsystems make infrequent allocations and manage the memory they need
 * internally.
 */

#include <mazu/arena.h>
#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/buddy.h>
#include <mazu/error.h>
#include <mazu/kvalloc.h>
#include <mazu/paging.h>
#include <mazu/pcpu.h>
#include <mazu/spinlock.h>

struct kvalloc {
    struct buddy *virt_alloc; /* Manages virtual pages handed out by this
                               * allocator.
                               */
};

/* Cannot dynamically allocate memory for these structures because they are
 * needed to initialize kvalloc, the dynamic allocator.
 */

/* An instance of a buddy allocator requires some memory for the heads of its
 * free lists and for the bitmaps it uses. The amount of memory required depends
 * on the size of the managed region because the bitmap increases in size with
 * bigger regions.
 */
#define VIRT_ALLOC_BACKING_MEM_SIZE 0x5000
static byte virt_alloc_backing_mem[VIRT_ALLOC_BACKING_MEM_SIZE];

static struct kvalloc global_kvalloc;
static bool global_kvalloc_is_initialized = false;
static spinlock_t kvalloc_lock = SPINLOCK_INITIALIZER;

static inline void kvalloc_assert_thread_context(void)
{
    DEBUG_ASSERT(!in_interrupt_context());
}

struct result kvalloc_init(struct byte_array vaddrs)
{
    assert(!global_kvalloc_is_initialized);

    struct arena arn = arena_new(
        byte_array_new(virt_alloc_backing_mem, VIRT_ALLOC_BACKING_MEM_SIZE));
    global_kvalloc.virt_alloc = buddy_init(vaddrs, &arn);

    global_kvalloc_is_initialized = true;

    return result_ok();
}

struct option_byte_array kvalloc_alloc(sz n_bytes, sz align)
{
    assert(global_kvalloc_is_initialized);
    assert(align <= PAGE_SIZE);
    kvalloc_assert_thread_context();

    sz real_size = ALIGN_UP(n_bytes, PAGE_SIZE);

    u64 flags = spin_lock_irqsave(&kvalloc_lock);
    struct option_byte_array result =
        buddy_alloc(global_kvalloc.virt_alloc, real_size);
    spin_unlock_irqrestore(&kvalloc_lock, flags);

    return result;
}

void kvalloc_free(struct byte_array ba)
{
    assert(global_kvalloc_is_initialized);
    kvalloc_assert_thread_context();

    if (!ba.dat)
        return;

    ba.len = ALIGN_UP(ba.len, PAGE_SIZE);

    u64 flags = spin_lock_irqsave(&kvalloc_lock);
    buddy_free(global_kvalloc.virt_alloc, ba);
    spin_unlock_irqrestore(&kvalloc_lock, flags);
}

void *kvalloc_alloc_wrapper(void *a __unused, sz size, sz align)
{
    struct option_byte_array ba = kvalloc_alloc(size, align);
    if (ba.is_none)
        return NULL;
    return byte_array_ptr(option_byte_array_checked(ba));
}

void kvalloc_free_wrapper(void *a __unused, void *ptr, sz size)
{
    kvalloc_free(byte_array_new(ptr, size));
}

struct kvalloc_stats kvalloc_stats_get(void)
{
    assert(global_kvalloc_is_initialized);
    struct buddy_stats bs = buddy_stats_get(global_kvalloc.virt_alloc);
    return (struct kvalloc_stats) {
        .total_pages = bs.total_pages,
        .free_pages = bs.free_pages,
    };
}
