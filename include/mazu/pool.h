/* SPDX-License-Identifier: MIT */
#ifndef MAZU_POOL_H
#define MAZU_POOL_H

#include <mazu/assert.h>
#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/spinlock.h>

struct pool {
    struct byte_array mem;
    ptr *head;
    sz size;
    spinlock_t lock;
};

/* Create a new pool allocator that uses the byte array 'mem' as its source of
 * memory.
 */
static inline struct pool pool_new(struct byte_array mem, sz block_size)
{
    struct pool pool;
    pool.head = NULL;
    pool.size = 0;
    pool.lock = (spinlock_t) SPINLOCK_INITIALIZER;

    sz align = MAX(sizeof(ptr), alignof(void *));
    ptr *block = NULL;
    sz n_blocks = 0;

    assert(mem.dat);
    assert(mem.len > 0);
    assert(block_size > 0);

    assert(align <= SZ_MAX - block_size);
    block_size = (block_size + align - 1) & ~(align - 1);

    assert(block_size > 0);
    n_blocks = mem.len / block_size;

    for (sz i = 0; i < n_blocks * block_size; i += block_size) {
        block = (ptr *) (mem.dat + i);
        *block = (ptr) pool.head;
        pool.head = block;
    }

    pool.mem = mem;
    pool.size = block_size;

    return pool;
}

/* Allocate a block from the pool. Returns 'NULL' if the pool is empty. */
static inline void *pool_alloc(struct pool *pool)
{
    assert(pool);

    u64 flags = spin_lock_irqsave(&pool->lock);

    if (!pool->head) {
        spin_unlock_irqrestore(&pool->lock, flags);
        return NULL;
    }

    ptr *block = pool->head;
    pool->head = (ptr *) *block;

    spin_unlock_irqrestore(&pool->lock, flags);

    byte_array_set(byte_array_new(block, pool->size), 0);

    return block;
}

/* Free a block from the pool. Passing 'NULL' in the 'block' pointer is fine, it
 * will be ignored.
 */
static inline void pool_free(struct pool *pool, void *block)
{
    assert(pool);

    if (!block)
        return;

    assert(IN_RANGE((ptr) block, (ptr) pool->mem.dat, pool->mem.len));
    assert(((ptr) block - (ptr) pool->mem.dat) % pool->size == 0);

    u64 flags = spin_lock_irqsave(&pool->lock);
    *(ptr *) block = (ptr) pool->head;
    pool->head = block;
    spin_unlock_irqrestore(&pool->lock, flags);
}

/* Type-safe pool allocation macros.
 * POOL_TYPED_ALLOC casts the void* return to the correct type.
 * POOL_TYPED_FREE validates the magic number (Q2) before returning
 * the block to the pool.  The magic constant is passed explicitly
 * because type names (struct tcp_conn) do not map cleanly to constant
 * names (TCP_MAGIC) via token pasting.
 */
#define POOL_TYPED_ALLOC(type, pool) ((type *) pool_alloc(pool))

#define POOL_TYPED_FREE(pool, ptr, magic_val) \
    do {                                      \
        MAGIC_CHECK((ptr), (magic_val));      \
        pool_free((pool), (ptr));             \
    } while (0)

#endif /* MAZU_POOL_H */
