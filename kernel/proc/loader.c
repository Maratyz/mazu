/* SPDX-License-Identifier: MIT */
/* Binary loader for user-space processes. */

#include <mazu/paging.h>
#include <mazu/print.h>
#include <mazu/proc.h>
#include <mazu/string.h>
#include <mazu/uaccess.h>
#include "elf64.h"

struct result proc_load_flat(struct proc *p, struct byte_view binary)
{
    ALWAYS_ASSERT(p != NULL);

    if (binary.dat == NULL || binary.len <= 0) {
        pr_debug(STR("proc_load_flat: invalid binary\n"));
        return result_error(EINVAL);
    }

    sz n_pages = (binary.len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (n_pages == 0 || n_pages > PROC_PAGES_MAX) {
        pr_debug(STR("proc_load_flat: invalid page count %ld\n"), n_pages);
        return result_error(EINVAL);
    }

    /* Use per-process VA base so concurrent processes don't overlap. */
    vaddr_t code_base = p->va_code_base;

    /* Map code pages as R+W during loading (W^X: cannot be W+X). */
    for (sz i = 0; i < n_pages; i++) {
        struct result r = proc_map_user_page(p, code_base + i * PAGE_SIZE,
                                             PT_FLAG_USER | PT_FLAG_RW);
        if (r.is_error) {
            pr_debug(STR("loader: Failed to map code page %zd at 0x%lx: "
                         "err=%d\n"),
                     i, code_base + i * PAGE_SIZE, r.code);
            return r;
        }
    }

    /* Register code VMA before copy so VMA validation passes if n_vmas > 0.
     * Permissions are the final R+X; the page-table permissions (R+W during
     * loading) are independent of VMA permissions.
     */
    i32 vma_rc = proc_add_vma(p, code_base, n_pages * PAGE_SIZE,
                              VMA_PERM_READ | VMA_PERM_EXEC);
    if (vma_rc < 0)
        return result_error(ENOMEM);

    /* Copy binary data into user pages.
     * SUM must be set to write into them from S-mode.
     */
    i64 rc = copy_to_user_proc(p, (ptr) code_base, binary.dat, binary.len);
    if (rc < 0) {
        pr_debug(STR("proc_load_flat: copy_to_user failed with %lld\n"), rc);
        return result_error(EFAULT);
    }

    /* W^X transition: code pages loaded as R+W, now set to R+X. */
    for (sz i = 0; i < n_pages; i++) {
        paging_update_page_perms(code_base + i * PAGE_SIZE,
                                 PT_FLAG_USER | PT_FLAG_EXEC);
    }

    return result_ok();
}

struct result proc_load_elf(struct proc *p, struct byte_view binary)
{
    ALWAYS_ASSERT(p != NULL);

    if (binary.dat == NULL || binary.len < (sz) sizeof(struct elf64_hdr)) {
        pr_debug(
            STR("proc_load_elf: binary too small for ELF "
                "header\n"));
        return result_error(EINVAL);
    }

    struct elf64_hdr *hdr = (struct elf64_hdr *) binary.dat;
    if (!elf64_is_valid(hdr)) {
        pr_warn(STR("proc_load_elf: invalid ELF header\n"));
        return result_error(EINVAL);
    }

    /* Validate ELF header field bounds before accessing program headers. */
    if (hdr->phdr_count <= 0 || hdr->phdr_count > 1024) {
        pr_debug(STR("proc_load_elf: invalid phdr_count %d\n"),
                 hdr->phdr_count);
        return result_error(EINVAL);
    }

    if (hdr->phdr_size != sizeof(struct elf64_phdr)) {
        pr_debug(STR("proc_load_elf: unexpected phdr_size %d\n"),
                 hdr->phdr_size);
        return result_error(EINVAL);
    }

    /* Validate phdr_tab_offset against binary length. */
    if (hdr->phdr_tab_offset > (u64) binary.len - sizeof(struct elf64_phdr)) {
        pr_debug(STR("proc_load_elf: invalid phdr_tab_offset\n"));
        return result_error(EINVAL);
    }

    /* Map PT_LOAD segments. */
    for (u16 i = 0; i < hdr->phdr_count; i++) {
        sz off = (sz) hdr->phdr_tab_offset + (sz) i * hdr->phdr_size;

        /* Bounds-check program header access. */
        if (off + sizeof(struct elf64_phdr) > (sz) binary.len) {
            pr_debug(STR("proc_load_elf: phdr %u out of bounds\n"), i);
            return result_error(EINVAL);
        }

        struct elf64_phdr *ph = (struct elf64_phdr *) (binary.dat + off);
        if (ph->type != PT_LOAD)
            continue;
        if (ph->mem_size == 0)
            continue;

        /* Reject oversized segments. */
        if (ph->mem_size > 64UL * 1024 * 1024) {
            pr_debug(STR("proc_load_elf: invalid mem_size for segment %u\n"),
                     i);
            return result_error(EINVAL);
        }

        /* Reject W+X segments (W^X policy). */
        if ((ph->flags & 0x3) == 0x3) { /* PF_X | PF_W */
            pr_debug(STR("proc_load_elf: segment %u requests W+X (rejected)\n"),
                     i);
            return result_error(EINVAL);
        }

        /* Determine final page permissions from ELF flags. */
        u16 final_perms = PT_FLAG_USER;
        if (ph->flags & 0x2) /* PF_W */
            final_perms |= PT_FLAG_RW;
        if (ph->flags & 0x1) /* PF_X */
            final_perms |= PT_FLAG_EXEC;

        /* Compute page-aligned segment bounds in u64 to avoid sign-wrap.
         * A malicious ELF with p_vaddr >= 2^63 would wrap negative if cast
         * to signed vaddr_t before validation.
         */
        u64 seg_base_u = ph->vaddr & ~(u64) (PAGE_SIZE - 1);

        /* Detect overflow in vaddr + mem_size (and the subsequent
         * page-alignment round-up which adds PAGE_SIZE - 1).
         */
        if (ph->mem_size > U64_MAX - ph->vaddr ||
            ph->vaddr + ph->mem_size > U64_MAX - (PAGE_SIZE - 1)) {
            pr_debug(STR("proc_load_elf: segment %u overflows\n"), i);
            return result_error(EINVAL);
        }
        u64 seg_end_u =
            (ph->vaddr + ph->mem_size + PAGE_SIZE - 1) & ~(u64) (PAGE_SIZE - 1);

        /* Segment must fall within this process's VA window. */
        if (seg_base_u < (u64) p->va_code_base ||
            seg_end_u > (u64) p->va_stack_top) {
            pr_debug(STR("proc_load_elf: segment %u out of process VA range\n"),
                     i);
            return result_error(EINVAL);
        }

        /* Range proven valid; safe to narrow to vaddr_t. */
        vaddr_t seg_base = (vaddr_t) seg_base_u;
        vaddr_t seg_end = (vaddr_t) seg_end_u;

        /* Detect degenerate empty range. */
        if (seg_end <= seg_base) {
            pr_debug(STR("proc_load_elf: invalid segment range for %u\n"), i);
            return result_error(EINVAL);
        }

        /* W^X: map all segments as R+W during loading; executable
         * segments will be transitioned to R+X after data is copied.
         */
        u16 load_perms = PT_FLAG_USER | PT_FLAG_RW;
        for (vaddr_t va = seg_base; va < seg_end; va += PAGE_SIZE) {
            struct result r = proc_map_user_page(p, va, load_perms);
            if (r.is_error) {
                pr_debug(STR("loader: Failed to map ELF segment %u at 0x%lx: "
                             "err=%d\n"),
                         i, va, r.code);
                return r;
            }
        }

        /* Register VMA for this segment. */
        u16 vma_perms = VMA_PERM_READ;
        if (ph->flags & 0x2) /* PF_W */
            vma_perms |= VMA_PERM_WRITE;
        if (ph->flags & 0x1) /* PF_X */
            vma_perms |= VMA_PERM_EXEC;
        i32 vma_rc = proc_add_vma(p, seg_base, seg_end - seg_base, vma_perms);
        if (vma_rc < 0)
            return result_error(ENOMEM);

        /* Copy file content. */
        if (ph->file_size > 0) {
            /* Validate file_size and offset bounds. */
            if (ph->file_size > ph->mem_size) {
                pr_debug(STR("proc_load_elf: invalid file_size for segment "
                             "%u\n"),
                         i);
                return result_error(EINVAL);
            }

            if (ph->offset > (u64) binary.len ||
                ph->file_size > (u64) binary.len - ph->offset) {
                pr_debug(STR("proc_load_elf: segment %u extends beyond "
                             "binary\n"),
                         i);
                return result_error(EINVAL);
            }

            i64 rc =
                copy_to_user_proc(p, (ptr) ph->vaddr, binary.dat + ph->offset,
                                  (sz) ph->file_size);
            if (rc < 0) {
                pr_debug(STR("loader: copy_to_user failed for segment %u at "
                             "0x%lx: err=%lld\n"),
                         i, ph->vaddr, rc);
                return result_error(EFAULT);
            }
        }

        /* Permission transition: all segments were loaded as R+W; now set
         * their final permissions.  This covers R+X (code), R-only (rodata),
         * and leaves R+W (data/bss) unchanged.
         */
        if (final_perms != load_perms) {
            for (vaddr_t va = seg_base; va < seg_end; va += PAGE_SIZE)
                paging_update_page_perms(va, final_perms);
        }
    }

    return result_ok();
}

#include __INC_TEST(loader)
