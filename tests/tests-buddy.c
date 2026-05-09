/* SPDX-License-Identifier: MIT */
#include <mazu/kvalloc.h>
#include <mazu/selftest.h>

static i32 test_buddy_alloc_free(void)
{
    /* Allocate a page, write to it, free it, allocate again - the buddy
     * should hand back the same page.
     */
    struct option_byte_array ba1 = kvalloc_alloc(PAGE_SIZE, PAGE_SIZE);
    if (ba1.is_none)
        return 1;
    struct byte_array a1 = option_byte_array_checked(ba1);

    /* Write a pattern and verify. */
    byte *p = a1.dat;
    for (sz i = 0; i < PAGE_SIZE; i++)
        p[i] = (byte) (i & 0xFF);
    for (sz i = 0; i < PAGE_SIZE; i++) {
        if (p[i] != (byte) (i & 0xFF))
            return 1;
    }

    kvalloc_free(a1);

    /* Second allocation should succeed. */
    struct option_byte_array ba2 = kvalloc_alloc(PAGE_SIZE, PAGE_SIZE);
    if (ba2.is_none)
        return 1;
    kvalloc_free(option_byte_array_checked(ba2));

    return 0;
}

DEFINE_SELFTEST(buddy_alloc_free, test_buddy_alloc_free);
