/* SPDX-License-Identifier: MIT */
/* User-space memory access with sstatus.SUM toggling. */

#include <isr.h>
#include <mazu/asm.h>
#include <mazu/base.h>
#include <mazu/paging.h>
#include <mazu/pcpu.h>
#include <mazu/proc.h>
#include <mazu/sched.h>
#include <mazu/string.h>
#include <mazu/uaccess.h>

/* Common range check for user address validation.  Returns false if the
 * address wraps, lies outside the user VA range, or (when VMAs are
 * registered) falls outside a VMA with the required permissions.
 */
static bool user_addr_range_ok(ptr addr, sz len, u16 vma_perms)
{
    uptr uaddr = (uptr) addr;

    if (len == 0)
        return true;
    if (uaddr > (uptr) (U64_MAX - len))
        return false;
    uptr end = uaddr + (uptr) len;
    if (uaddr < (uptr) USER_CODE_BASE || end > (uptr) USER_STACK_TOP)
        return false;

    struct sched_task *td = sched_current_task();
    if (td && td->proc && td->proc->n_vmas > 0) {
        if (!proc_vma_check_access(td->proc, addr, len, vma_perms))
            return false;
    }
    return true;
}

static inline bool user_addr_single_page(ptr addr, sz len, u64 *page_out)
{
    if (len == 0)
        return true;

    u64 start = ALIGN_DOWN((u64) (uptr) addr, PAGE_SIZE);
    u64 last = (u64) (uptr) addr + (u64) len - 1;
    if (last < (u64) (uptr) addr)
        return false;
    u64 end = ALIGN_DOWN(last, PAGE_SIZE);
    if (page_out)
        *page_out = start;
    return start == end;
}

static bool user_addr_validate_cached(ptr addr, sz len, bool writable)
{
    u16 perms = writable ? VMA_PERM_WRITE : VMA_PERM_READ;
    if (!user_addr_range_ok(addr, len, perms))
        return false;
    if (len == 0)
        return true;

    struct pcpu *pc = get_pcpu();
    u64 page;
    if (user_addr_single_page(addr, len, &page)) {
        u64 gen = paging_user_validation_gen();
        u64 *cached_page =
            writable ? &pc->uaccess_write_page : &pc->uaccess_read_page;
        u64 *cached_gen =
            writable ? &pc->uaccess_write_gen : &pc->uaccess_read_gen;
        if (*cached_gen == gen && *cached_page == page)
            return true;

        bool ok = writable ? paging_user_range_writable((vaddr_t) addr, len)
                           : paging_user_range_accessible((vaddr_t) addr, len);
        if (ok) {
            *cached_page = page;
            *cached_gen = gen;
        }
        return ok;
    }

    return writable ? paging_user_range_writable((vaddr_t) addr, len)
                    : paging_user_range_accessible((vaddr_t) addr, len);
}

static inline void uaccess_copy_small_to_user(ptr udst, const void *src, sz len)
{
    const u8 *s = src;
    volatile u8 *d = (volatile u8 *) udst;

    switch (len) {
    case 16:
        d[15] = s[15];
        __fallthrough;
    case 15:
        d[14] = s[14];
        __fallthrough;
    case 14:
        d[13] = s[13];
        __fallthrough;
    case 13:
        d[12] = s[12];
        __fallthrough;
    case 12:
        d[11] = s[11];
        __fallthrough;
    case 11:
        d[10] = s[10];
        __fallthrough;
    case 10:
        d[9] = s[9];
        __fallthrough;
    case 9:
        d[8] = s[8];
        __fallthrough;
    case 8:
        d[7] = s[7];
        __fallthrough;
    case 7:
        d[6] = s[6];
        __fallthrough;
    case 6:
        d[5] = s[5];
        __fallthrough;
    case 5:
        d[4] = s[4];
        __fallthrough;
    case 4:
        d[3] = s[3];
        __fallthrough;
    case 3:
        d[2] = s[2];
        __fallthrough;
    case 2:
        d[1] = s[1];
        __fallthrough;
    case 1:
        d[0] = s[0];
        __fallthrough;
    default:
        break;
    }
}

static inline void uaccess_copy_small_from_user(void *dst, ptr usrc, sz len)
{
    u8 *d = dst;
    volatile const u8 *s = (volatile const u8 *) usrc;

    switch (len) {
    case 16:
        d[15] = s[15];
        __fallthrough;
    case 15:
        d[14] = s[14];
        __fallthrough;
    case 14:
        d[13] = s[13];
        __fallthrough;
    case 13:
        d[12] = s[12];
        __fallthrough;
    case 12:
        d[11] = s[11];
        __fallthrough;
    case 11:
        d[10] = s[10];
        __fallthrough;
    case 10:
        d[9] = s[9];
        __fallthrough;
    case 9:
        d[8] = s[8];
        __fallthrough;
    case 8:
        d[7] = s[7];
        __fallthrough;
    case 7:
        d[6] = s[6];
        __fallthrough;
    case 6:
        d[5] = s[5];
        __fallthrough;
    case 5:
        d[4] = s[4];
        __fallthrough;
    case 4:
        d[3] = s[3];
        __fallthrough;
    case 3:
        d[2] = s[2];
        __fallthrough;
    case 2:
        d[1] = s[1];
        __fallthrough;
    case 1:
        d[0] = s[0];
        __fallthrough;
    default:
        break;
    }
}

bool user_addr_valid(ptr addr, sz len)
{
    return user_addr_validate_cached(addr, len, false);
}

bool user_addr_writable(ptr addr, sz len)
{
    return user_addr_validate_cached(addr, len, true);
}

static void uaccess_begin(u64 recover_sepc)
{
    struct pcpu *pc = get_pcpu();
    pc->uaccess_faulted = false;
    pc->uaccess_recover_sepc = recover_sepc;
    pc->uaccess_active = true;
}

static i64 uaccess_end(void)
{
    struct pcpu *pc = get_pcpu();
    bool faulted = pc->uaccess_faulted;
    pc->uaccess_active = false;
    pc->uaccess_recover_sepc = 0;
    pc->uaccess_faulted = false;
    uaccess_disable();
    return faulted ? -(i64) EFAULT : 0;
}

i64 copy_to_user(ptr udst, const void *src, sz len)
{
    i64 rc = 0;

    if (!user_addr_writable(udst, len))
        return -(i64) EFAULT;
    if (len == 0)
        return 0;

    uaccess_begin((u64) (uptr) __extension__ && fault_recover);
    /* Set SUM so S-mode can access U-mode pages. Faults are recovered in
     * trap_dispatch via uaccess_handle_page_fault().
     */
    uaccess_enable();
    if (len <= 16)
        uaccess_copy_small_to_user(udst, src, len);
    else
        memcpy((void *) udst, src, len);
    goto out;

fault_recover:
    rc = -(i64) EFAULT;

out:
    if (uaccess_end() < 0)
        rc = -(i64) EFAULT;
    return rc;
}

i64 copy_from_user(void *dst, ptr usrc, sz len)
{
    i64 rc = 0;

    if (!user_addr_valid(usrc, len))
        return -(i64) EFAULT;
    if (len == 0)
        return 0;

    uaccess_begin((u64) (uptr) __extension__ && fault_recover);
    uaccess_enable();
    if (len <= 16)
        uaccess_copy_small_from_user(dst, usrc, len);
    else
        memcpy(dst, (const void *) usrc, len);
    goto out;

fault_recover:
    rc = -(i64) EFAULT;

out:
    if (uaccess_end() < 0)
        rc = -(i64) EFAULT;
    return rc;
}

/* Validate destination against page-table permissions (not VMAs).
 * Used by the loader to write into a child's address space during spawn.
 * VMA permission is deliberately skipped: the loader has already validated
 * the address against the child's VA window bounds, and the VMA records
 * final (post-load) permissions while PTEs have temporary R+W during load.
 */
static bool user_addr_writable_proc(struct proc *p, ptr addr, sz len)
{
    uptr uaddr = (uptr) addr;
    if (len == 0)
        return true;
    if (uaddr > (uptr) (U64_MAX - len))
        return false;
    uptr end = uaddr + (uptr) len;

    /* Address must be within the child's VA window. */
    if (p) {
        if (uaddr < (uptr) p->va_code_base || end > (uptr) p->va_stack_top)
            return false;
    } else {
        if (uaddr < (uptr) USER_CODE_BASE || end > (uptr) USER_STACK_TOP)
            return false;
    }
    return paging_user_range_writable((vaddr_t) addr, len);
}

i64 copy_to_user_proc(struct proc *p, ptr udst, const void *src, sz len)
{
    i64 rc = 0;

    if (!user_addr_writable_proc(p, udst, len))
        return -(i64) EFAULT;
    if (len == 0)
        return 0;

    uaccess_begin((u64) (uptr) __extension__ && fault_recover_proc);
    uaccess_enable();
    if (len <= 16)
        uaccess_copy_small_to_user(udst, src, len);
    else
        memcpy((void *) udst, src, len);
    goto out;

fault_recover_proc:
    rc = -(i64) EFAULT;

out:
    if (uaccess_end() < 0)
        rc = -(i64) EFAULT;
    return rc;
}

bool uaccess_handle_page_fault(struct trap_frame *tf)
{
    struct pcpu *pc = get_pcpu();
    if (!pc->uaccess_active || pc->uaccess_recover_sepc == 0)
        return false;

    pc->uaccess_faulted = true;
    pc->uaccess_active = false;
    tf->sepc = pc->uaccess_recover_sepc;
    /* Ensure SUM is cleared before returning to fault-recovery code. */
    uaccess_disable();
    return true;
}

#include __INC_TEST(uaccess)
