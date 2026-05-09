/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit paging - Sv39 three-level identity-mapped page tables.
 *
 * Builds identity-mapped (virt == phys) Sv39 page tables using 2 MiB
 * superpages.  Page-table pages are stolen from the front of the dynamic
 * region before kvalloc_init.
 *
 * The software addr_mapping table is retained so that virt_to_phys /
 * phys_to_virt work for DMA address conversion.
 */

/* Page table code converts physical addresses to pointers throughout. */

#include <fdt.h>
#include <mazu/asm.h>
#include <mazu/assert.h>
#include <mazu/byte.h>
#include <mazu/ipi.h>
#include <mazu/paging.h>
#include <mazu/print.h>
#include <mazu/spinlock.h>

/* Sv39 PTE format */

typedef u64 pte_t;

#define PTE_V BIT(0) /* valid */
#define PTE_R BIT(1) /* read */
#define PTE_W BIT(2) /* write */
#define PTE_X BIT(3) /* execute */
#define PTE_U BIT(4) /* user-accessible */
#define PTE_G BIT(5) /* global (no ASID flush needed) */
#define PTE_A BIT(6) /* accessed */
#define PTE_D BIT(7) /* dirty */

#define PTE_PPN_SHIFT 10
#define PT_ENTRIES 512
#define SV39_SUPERPAGE 0x200000ULL /* 2 MiB */
#define SATP_MODE_SV39 (8ULL << 60)

/* Kernel leaf PTE flags.  A and D pre-set to avoid hardware faults. */
#define PTE_KERN_RWX (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PTE_KERN_RW (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)

static inline pte_t pte_make(u64 paddr, u64 flags)
{
    assert(paddr < (1ULL << 56));
    assert(IS_ALIGNED(paddr, PAGE_SIZE));
    return ((paddr >> 12) << PTE_PPN_SHIFT) | flags;
}

static inline u64 pte_ppn_addr(pte_t pte)
{
    /* Mask the 44-bit PPN field so reserved/PBMT/N bits [63:54] never leak into
     * the reconstructed physical address.
     */
    return (((pte >> PTE_PPN_SHIFT) & 0xFFFFFFFFFFFULL) << 12);
}

static inline bool pte_is_leaf(pte_t pte)
{
    return (pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X));
}

/* Sv39 virtual address decomposition. */
static inline u64 vpn2(u64 va)
{
    return (va >> 30) & 0x1FF;
}
static inline u64 vpn1(u64 va)
{
    return (va >> 21) & 0x1FF;
}
static inline u64 vpn0(u64 va)
{
    return (va >> 12) & 0x1FF;
}

/* Page-table page pool
 *
 * Steals pages from the front of the dynamic region. The pool is sized for the
 * initial mapping (~4 pages) plus spare pages for later L0 shattering when
 * guard pages unmap individual 4 KiB pages.
 */

/* 128 KiB: ~4 init + spare for shattering + user L0 tables */
#define PT_POOL_PAGES 32

static u64 pt_pool_base;
static u64 pt_pool_cursor;
static u64 pt_pool_end;
static spinlock_t pt_lock = SPINLOCK_INITIALIZER;
static u64 user_validation_gen = 1;

static pte_t *pt_alloc_page(void)
{
    assert(pt_pool_cursor + PAGE_SIZE <= pt_pool_end);
    pte_t *page = (pte_t *) pt_pool_cursor;
    for (u64 i = 0; i < PT_ENTRIES; i++)
        page[i] = 0;
    pt_pool_cursor += PAGE_SIZE;
    return page;
}

/* Root page table (L2), set during paging_init. */
static pte_t *root_pt;

/* Page table manipulation */

/* Ensure vpn[2] slot in the root table points to an L1 table. */
static void sv39_ensure_l1(pte_t *root, u64 idx)
{
    if (!(root[idx] & PTE_V)) {
        pte_t *l1 = pt_alloc_page();
        root[idx] = pte_make((u64) l1, PTE_V);
    }
}

/* Install a 2 MiB superpage identity mapping (vaddr == paddr). */
static void sv39_map_2m(pte_t *root, u64 addr, u64 flags)
{
    assert(IS_ALIGNED(addr, SV39_SUPERPAGE));
    u64 i2 = vpn2(addr);
    sv39_ensure_l1(root, i2);
    pte_t *l1 = (pte_t *) pte_ppn_addr(root[i2]);
    l1[vpn1(addr)] = pte_make(addr, flags);
}

/* Shatter a 2 MiB superpage into 512 individual 4 KiB PTEs. */
static void sv39_shatter_2m(pte_t *l1_entry)
{
    assert(pte_is_leaf(*l1_entry));
    u64 base = pte_ppn_addr(*l1_entry);
    u64 flags = *l1_entry & ((1ULL << PTE_PPN_SHIFT) - 1);
    pte_t *l0 = pt_alloc_page();
    for (u64 k = 0; k < PT_ENTRIES; k++)
        l0[k] = pte_make(base + k * PAGE_SIZE, flags);
    *l1_entry = pte_make((u64) l0, PTE_V);
}

/* Walk to the L0 PTE for a 4 KiB page. Creates intermediate tables on demand so
 * that user-space addresses outside the kernel identity map can be mapped
 * without a prior 2 MiB superpage.
 */
static pte_t *sv39_walk_l0(pte_t *root, u64 vaddr)
{
    u64 i2 = vpn2(vaddr);
    u64 i1 = vpn1(vaddr);

    /* Ensure L1 table exists (may create for user-space vpn2 regions). */
    sv39_ensure_l1(root, i2);
    pte_t *l1 = (pte_t *) pte_ppn_addr(root[i2]);

    if (pte_is_leaf(l1[i1])) {
        sv39_shatter_2m(&l1[i1]);
        /* Flush the stale 2 MiB TLB entry on this hart and broadcast to others.
         * A 4 KiB sfence.vma may not invalidate a megapage entry on hardware
         * with split TLBs, so use the 2 MiB-aligned base.
         *
         * The IPI is asynchronous (fire-and-forget).  This is safe because
         * shattering only occurs during task setup (guard page creation, user
         * page mapping) before the affected address range is accessed by tasks
         * on other harts. The shattered 4 KiB pages initially map the same
         * physical addresses with the same permissions as the original
         * superpage, so a stale 2 MiB TLB entry produces correct translations
         * until the remote hart processes the IPI.
         */
        u64 mega_base = vaddr & ~((u64) SV39_SUPERPAGE - 1);
        __asm__ volatile("sfence.vma %0, zero" : : "r"(mega_base) : "memory");
        ipi_send_broadcast(IPI_TLB);
    } else if (!(l1[i1] & PTE_V)) {
        /* Allocate an L0 table for a fresh user-space mapping range. */
        pte_t *l0 = pt_alloc_page();
        l1[i1] = pte_make((u64) l0, PTE_V);
    }

    pte_t *l0 = (pte_t *) pte_ppn_addr(l1[i1]);
    return &l0[vpn0(vaddr)];
}

/* Lookup the leaf PTE (L1 superpage or L0 page) for vaddr without allocating
 * any page-table pages.
 */
static bool sv39_lookup_leaf(pte_t *root, u64 vaddr, pte_t *leaf)
{
    u64 i2 = vpn2(vaddr);
    u64 i1 = vpn1(vaddr);

    pte_t e2 = root[i2];
    if (!(e2 & PTE_V))
        return false;

    pte_t *l1 = (pte_t *) pte_ppn_addr(e2);
    pte_t e1 = l1[i1];
    if (!(e1 & PTE_V))
        return false;

    if (pte_is_leaf(e1)) {
        *leaf = e1;
        return true;
    }

    pte_t *l0 = (pte_t *) pte_ppn_addr(e1);
    pte_t e0 = l0[vpn0(vaddr)];
    if (!(e0 & PTE_V) || !pte_is_leaf(e0))
        return false;

    *leaf = e0;
    return true;
}

/* Write satp and flush TLB.  Interrupts disabled across the transition. */
static void sv39_activate(pte_t *root)
{
    u64 satp_val = SATP_MODE_SV39 | ((u64) root >> 12);
    disable_interrupts();
    __asm__ volatile(
        /* Flush stale TLB entries from prior boot stages (OpenSBI). */
        "sfence.vma zero, zero\n"
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(satp_val)
        : "memory");
    enable_interrupts();
}

/* Flush a single TLB entry. */
static inline void sfence_vma_page(u64 vaddr)
{
    __asm__ volatile("sfence.vma %0, zero" : : "r"(vaddr) : "memory");
}

/* Virtual <-> physical address mapping table (software, for DMA) */

#define NUM_ADDR_MAPPINGS 32
static struct addr_mapping global_addr_mappings[NUM_ADDR_MAPPINGS];
static bool global_addr_mappings_used[NUM_ADDR_MAPPINGS];

static inline bool intervals_overlap(sz a1, sz b1, sz a2, sz b2)
{
    return a1 < b2 && a2 < b1;
}

static struct result add_addr_mapping(struct addr_mapping new_mapping)
{
    for (i32 i = 0; i < NUM_ADDR_MAPPINGS; i++) {
        if (global_addr_mappings_used[i]) {
            struct addr_mapping *m = &global_addr_mappings[i];
            if (intervals_overlap(new_mapping.vbase,
                                  new_mapping.vbase + new_mapping.len, m->vbase,
                                  m->vbase + m->len))
                return result_error(EINVAL);
        }
    }

    for (i32 i = 0; i < NUM_ADDR_MAPPINGS; i++) {
        if (!global_addr_mappings_used[i]) {
            global_addr_mappings[i] = new_mapping;
            global_addr_mappings_used[i] = true;
            return result_ok();
        }
    }

    return result_error(ENOMEM);
}

/* Find the addr_mapping containing 'addr' in the given field (vbase or pbase).
 */
static struct addr_mapping *find_mapping(u64 addr, bool by_virt)
{
    struct addr_mapping *match = NULL;
    for (i32 i = 0; i < NUM_ADDR_MAPPINGS; i++) {
        if (!global_addr_mappings_used[i])
            continue;
        struct addr_mapping *m = &global_addr_mappings[i];
        u64 base = by_virt ? (u64) m->vbase : (u64) m->pbase;
        if (IN_RANGE(addr, base, m->len)) {
            assert(!match); /* at most one mapping per address */
            match = m;
        }
    }
    return match;
}

struct result_vaddr_t phys_to_virt(paddr_t paddr)
{
    if (!paddr)
        return result_vaddr_t_ok(0);
    struct addr_mapping *m = find_mapping((u64) paddr, false);
    if (m)
        return result_vaddr_t_ok(m->vbase + (paddr - m->pbase));
    return result_vaddr_t_error(EINVAL);
}

struct result_paddr_t virt_to_phys(vaddr_t vaddr)
{
    if (!vaddr)
        return result_paddr_t_ok(0);
    struct addr_mapping *m = find_mapping((u64) vaddr, true);
    if (m)
        return result_paddr_t_ok(m->pbase + (vaddr - m->vbase));
    return result_paddr_t_error(EINVAL);
}

/* Public paging interface */

struct byte_array paging_init(struct addr_mapping code_addrs,
                              struct addr_mapping dyn_addrs)
{
    /* Register identity mappings in the software table. */
    code_addrs.perms = PT_FLAG_RW;
    dyn_addrs.perms = PT_FLAG_RW;

    assert(!add_addr_mapping(code_addrs).is_error);
    assert(!add_addr_mapping(dyn_addrs).is_error);

    /* Reserve page-table pages from the front of the dynamic region. */
    pt_pool_base = (u64) dyn_addrs.vbase;
    pt_pool_cursor = pt_pool_base;
    pt_pool_end = pt_pool_base + (u64) PT_POOL_PAGES * PAGE_SIZE;
    assert(pt_pool_end <= (u64) dyn_addrs.vbase + (u64) dyn_addrs.len);

    root_pt = pt_alloc_page();

    /* Map the kernel code region with 2 MiB superpages. */
    u64 code_base = ALIGN_DOWN((u64) code_addrs.pbase, SV39_SUPERPAGE);
    u64 code_end =
        ALIGN_UP((u64) code_addrs.pbase + (u64) code_addrs.len, SV39_SUPERPAGE);
    for (u64 a = code_base; a < code_end; a += SV39_SUPERPAGE)
        sv39_map_2m(root_pt, a, PTE_KERN_RWX);

    /* Map the dynamic region with 2 MiB superpages. */
    u64 dyn_base = ALIGN_DOWN((u64) dyn_addrs.pbase, SV39_SUPERPAGE);
    u64 dyn_end =
        ALIGN_UP((u64) dyn_addrs.pbase + (u64) dyn_addrs.len, SV39_SUPERPAGE);
    for (u64 a = dyn_base; a < dyn_end; a += SV39_SUPERPAGE)
        sv39_map_2m(root_pt, a, PTE_KERN_RW);

    /* Map MMIO space for the PLIC, UART, and VirtIO windows. */
    u64 plic = board_info.plic_base ? board_info.plic_base : 0x0c000000UL;
    u64 plic_sp = ALIGN_DOWN(plic, SV39_SUPERPAGE);
    sv39_map_2m(root_pt, plic_sp, PTE_KERN_RW);
    sv39_map_2m(root_pt, plic_sp + SV39_SUPERPAGE, PTE_KERN_RW);

    u64 uart = board_info.uart_base ? board_info.uart_base : 0x10000000UL;
    u64 uart_sp = ALIGN_DOWN(uart, SV39_SUPERPAGE);
    sv39_map_2m(root_pt, uart_sp, PTE_KERN_RW);

    /* VirtIO slots - map if they fall on a different 2 MiB page.
     * Track mapped superpages to avoid duplicate L1 entries.
     */
    for (u32 i = 0; i < board_info.virtio_count; i++) {
        u64 vio = ALIGN_DOWN(board_info.virtio_base[i], SV39_SUPERPAGE);
        if (vio == uart_sp || vio == plic_sp || vio == plic_sp + SV39_SUPERPAGE)
            continue;
        /* Skip if this superpage overlaps the code or dynamic regions. */
        if (vio >= code_base && vio < code_end)
            continue;
        if (vio >= dyn_base && vio < dyn_end)
            continue;
        /* Skip if a prior VirtIO slot already mapped this superpage. */
        bool dup = false;
        for (u32 j = 0; j < i; j++) {
            if (ALIGN_DOWN(board_info.virtio_base[j], SV39_SUPERPAGE) == vio) {
                dup = true;
                break;
            }
        }
        if (!dup)
            sv39_map_2m(root_pt, vio, PTE_KERN_RW);
    }

    /* Activate Sv39 translation. */
    sv39_activate(root_pt);

    printk(KERN_INFO, STR("Sv39 paging enabled (identity map, %ld PT pages)\n"),
           (sz) ((pt_pool_cursor - pt_pool_base) / PAGE_SIZE));

    /* Return dynamic region past the PT page pool. */
    u64 trimmed = pt_pool_end;
    sz trimmed_len =
        (sz) ((u64) dyn_addrs.vbase + (u64) dyn_addrs.len - trimmed);
    return byte_array_new((byte *) trimmed, trimmed_len);
}

void paging_unmap_page(vaddr_t vaddr)
{
    assert(root_pt);
    assert(IS_ALIGNED(vaddr, PAGE_SIZE));
    spin_lock(&pt_lock);
    pte_t *pte = sv39_walk_l0(root_pt, (u64) vaddr);
    *pte = 0;
    __atomic_add_fetch(&user_validation_gen, 1, __ATOMIC_RELAXED);
    spin_unlock(&pt_lock);
    sfence_vma_page((u64) vaddr);
}

/* W^X enforcement: user-space pages must not be simultaneously writable and
 * executable.
 * Returns true if the combination is rejected.
 */
static bool wxn_reject(vaddr_t vaddr, u16 perms)
{
    if ((perms & PT_FLAG_USER) && (perms & PT_FLAG_RW) &&
        (perms & PT_FLAG_EXEC)) {
        printk(KERN_ERR, STR("paging: W^X violation at 0x%lx\n"), (u64) vaddr);
        return true;
    }
    return false;
}

/* Translate PT_FLAG_* permission bits into Sv39 PTE flags. */
static u64 perms_to_pte_flags(u16 perms)
{
    u64 flags = PTE_V | PTE_R | PTE_A | PTE_D;
    if (perms & PT_FLAG_USER)
        flags |= PTE_U;
    else
        flags |= PTE_G;
    if (perms & PT_FLAG_RW)
        flags |= PTE_W;
    if (perms & PT_FLAG_EXEC)
        flags |= PTE_X;
    return flags;
}

void paging_map_page(vaddr_t vaddr, paddr_t paddr, u16 perms)
{
    assert(root_pt);
    assert(IS_ALIGNED(vaddr, PAGE_SIZE));
    assert(IS_ALIGNED(paddr, PAGE_SIZE));

    if (wxn_reject(vaddr, perms))
        return;

    spin_lock(&pt_lock);
    pte_t *pte = sv39_walk_l0(root_pt, (u64) vaddr);
    *pte = pte_make((u64) paddr, perms_to_pte_flags(perms));
    __atomic_add_fetch(&user_validation_gen, 1, __ATOMIC_RELAXED);
    spin_unlock(&pt_lock);
    sfence_vma_page((u64) vaddr);
}

void paging_update_page_perms(vaddr_t vaddr, u16 perms)
{
    assert(root_pt);
    assert(IS_ALIGNED(vaddr, PAGE_SIZE));

    if (wxn_reject(vaddr, perms))
        return;

    spin_lock(&pt_lock);
    pte_t *pte = sv39_walk_l0(root_pt, (u64) vaddr);
    if (!(*pte & PTE_V)) {
        spin_unlock(&pt_lock);
        return; /* page not mapped */
    }

    *pte = pte_make(pte_ppn_addr(*pte), perms_to_pte_flags(perms));
    __atomic_add_fetch(&user_validation_gen, 1, __ATOMIC_RELAXED);
    spin_unlock(&pt_lock);
    sfence_vma_page((u64) vaddr);
}

/* Check that every page in [vaddr, vaddr+len) has the required PTE flags. */
static bool paging_user_range_check(vaddr_t vaddr, sz len, u64 required)
{
    if (len == 0)
        return true;
    if (!root_pt)
        return false;

    u64 start = (u64) vaddr;
    u64 end = start + (u64) len;
    if (end < start)
        return false;

    u64 page = ALIGN_DOWN(start, PAGE_SIZE);
    while (page < end) {
        pte_t leaf;
        if (!sv39_lookup_leaf(root_pt, page, &leaf))
            return false;
        if ((leaf & required) != required)
            return false;
        page += PAGE_SIZE;
    }
    return true;
}

bool paging_user_range_accessible(vaddr_t vaddr, sz len)
{
    return paging_user_range_check(vaddr, len, PTE_U);
}

bool paging_user_range_writable(vaddr_t vaddr, sz len)
{
    return paging_user_range_check(vaddr, len, PTE_U | PTE_W);
}

u64 paging_user_validation_gen(void)
{
    return __atomic_load_n(&user_validation_gen, __ATOMIC_RELAXED);
}

#if CONFIG_SEMIHOSTING
#include __INC_TEST(paging)
#endif
