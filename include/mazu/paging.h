/* SPDX-License-Identifier: MIT */
#ifndef MAZU_PAGING_H
#define MAZU_PAGING_H

#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/error.h>

/* Permission bits stored in addr_mapping.perms. Arch-specific paging code
 * maps these to the appropriate page-table entry flags (e.g. Sv39 for RISC-V).
 */
#define PT_FLAG_RW BIT(1)
#define PT_FLAG_USER BIT(2)
#define PT_FLAG_EXEC BIT(3)

typedef ptr vaddr_t;
typedef ptr paddr_t;

struct_result(vaddr_t, vaddr_t);
struct_result(paddr_t, paddr_t);

/* Specifies a linear mapping between a contiguous region of virtual and of
 * physical memory.
 */
struct addr_mapping {
    u16 perms; /* PT_FLAG_* permission bits */
    vaddr_t vbase;
    paddr_t pbase;
    sz len;
};

/* To call this function, the beginning of the address range specified by
 * 'dyn_addrs' must already be mapped. This is because this function will use
 * memory starting at 'dyn_addrs.vbase' for page table pages. Not all of the
 * 'dyn_addrs.len' bytes will be used for page table pages, so not all of them
 * need to be mapped before calling this function. Returns a contiguous region
 * of virtual addresses that can be dynamically allocated by the kernel.
 */
struct byte_array paging_init(struct addr_mapping code_addrs,
                              struct addr_mapping dyn_addrs);

/* Unmap a single 4 KiB page. Shatters the enclosing 2 MiB superpage if needed.
 * Used by guard pages to create an unmapped hole.
 */
void paging_unmap_page(vaddr_t vaddr);

/* Re-map a single 4 KiB page with the given permissions (PT_FLAG_* bits).
 * Shatters the enclosing superpage on demand if not already shattered.
 * Only R/W permissions are supported; mappings are always non-executable.
 */
void paging_map_page(vaddr_t vaddr, paddr_t paddr, u16 perms);

/* Update the permission flags of an already-mapped 4 KiB page without
 * changing the physical address.  Used by the loader to transition code
 * pages from R+W (loading) to R+X (execution) for W^X compliance.
 */
void paging_update_page_perms(vaddr_t vaddr, u16 perms);

/* Return true iff every page touched by [vaddr, vaddr + len) is mapped as a
 * user-accessible leaf PTE (PTE_U set).
 */
bool paging_user_range_accessible(vaddr_t vaddr, sz len);

/* Return true iff every page touched by [vaddr, vaddr + len) is mapped as a
 * user-accessible AND writable leaf PTE (PTE_U | PTE_W set).
 */
bool paging_user_range_writable(vaddr_t vaddr, sz len);

/* Monotonic generation bumped whenever user-visible mappings or permissions
 * change. Used to invalidate fast-path uaccess validation caches.
 */
u64 paging_user_validation_gen(void);

/* Translate between physical and virtual addresses. The translations are based
 * on the address mappings that were created by calls to 'paging_init'.
 */
struct result_paddr_t virt_to_phys(vaddr_t vaddr);
struct result_vaddr_t phys_to_virt(paddr_t paddr);

#endif /* MAZU_PAGING_H */
