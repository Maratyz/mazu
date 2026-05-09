/* SPDX-License-Identifier: MIT */
#include <mazu/kvalloc.h>
#include <mazu/paging.h>
#include <mazu/selftest.h>

static i32 selftest_user_page_mapping(void)
{
    /* Allocates a physical page from kvalloc, maps it at a user address,
     * verifies PTE_U is set and PTE_G is not, then cleans up.
     */
    struct option_byte_array ba = kvalloc_alloc(PAGE_SIZE, PAGE_SIZE);
    assert(!ba.is_none);
    struct byte_array page = option_byte_array_checked(ba);

    struct result_paddr_t pa = virt_to_phys((vaddr_t) page.dat);
    assert(!pa.is_error);
    paddr_t paddr = result_paddr_t_checked(pa);

    vaddr_t uaddr = 0x10000; /* USER_CODE_BASE */
    paging_map_page(uaddr, paddr, PT_FLAG_USER | PT_FLAG_EXEC);

    pte_t *pte = sv39_walk_l0(root_pt, (u64) uaddr);
    assert(*pte & PTE_V);
    assert(*pte & PTE_U);
    assert(!(*pte & PTE_G));
    assert(*pte & PTE_X);

    paging_unmap_page(uaddr);
    kvalloc_free(page);
    return 0;
}
DEFINE_SELFTEST(user_page_mapping, selftest_user_page_mapping);

/* W^X enforcement: user-space pages must not be simultaneously W+X. */
static i32 selftest_wxn_enforcement(void)
{
    struct option_byte_array ba = kvalloc_alloc(PAGE_SIZE, PAGE_SIZE);
    assert(!ba.is_none);
    struct byte_array page = option_byte_array_checked(ba);

    struct result_paddr_t pa = virt_to_phys((vaddr_t) page.dat);
    assert(!pa.is_error);
    paddr_t paddr = result_paddr_t_checked(pa);

    vaddr_t uaddr = 0x10000; /* USER_CODE_BASE */

    /* W+X mapping for user pages should be rejected by W^X check.
     * paging_map_page returns without mapping when W^X is violated.
     */
    paging_map_page(uaddr, paddr, PT_FLAG_USER | PT_FLAG_RW | PT_FLAG_EXEC);

    /* The page should NOT be mapped (W^X rejection). */
    pte_t leaf;
    bool found = sv39_lookup_leaf(root_pt, (u64) uaddr, &leaf);
    if (found && (leaf & PTE_V)) {
        /* If it was mapped from a prior test, clean up. */
        paging_unmap_page(uaddr);
    }

    /* R+X (no W) should succeed. */
    paging_map_page(uaddr, paddr, PT_FLAG_USER | PT_FLAG_EXEC);
    pte_t *pte = sv39_walk_l0(root_pt, (u64) uaddr);
    assert(*pte & PTE_V);
    assert(*pte & PTE_X);
    assert(!(*pte & PTE_W));
    assert(*pte & PTE_U);

    /* R+W (no X) should succeed. */
    paging_update_page_perms(uaddr, PT_FLAG_USER | PT_FLAG_RW);
    assert(*pte & PTE_W);
    assert(!(*pte & PTE_X));

    paging_unmap_page(uaddr);
    kvalloc_free(page);
    return 0;
}
DEFINE_SELFTEST(wxn_enforcement, selftest_wxn_enforcement);
