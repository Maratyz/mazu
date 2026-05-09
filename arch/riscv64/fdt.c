/* SPDX-License-Identifier: MIT */
/* Flattened Device Tree (FDT) parser.
 *
 * Sequential token walker that extracts board info from the DTB.
 * No dynamic allocation - works before any allocator is initialized.
 * Big-endian format (FDT is always big-endian regardless of CPU).
 */

#include <fdt.h>
#include <mazu/print.h>

/* FDT magic and token constants. */
#define FDT_MAGIC 0xd00dfeedU
#define FDT_MIN_VERSION 17
#define FDT_BEGIN_NODE 0x00000001U
#define FDT_END_NODE 0x00000002U
#define FDT_PROP 0x00000003U
#define FDT_NOP 0x00000004U
#define FDT_END 0x00000009U

/* Global board info. */
struct fdt_board_info board_info;

/* FDT address saved by _start (in .data so it survives BSS zeroing). */
u64 _fdt_addr __attribute__((section(".data"))) = 0;

/* Big-endian read helpers */

static inline u32 fdt32(const void *p)
{
    const u8 *b = p;
    return ((u32) b[0] << 24) | ((u32) b[1] << 16) | ((u32) b[2] << 8) |
           (u32) b[3];
}

static inline u64 fdt64(const void *p)
{
    return ((u64) fdt32(p) << 32) | (u64) fdt32((const u8 *) p + 4);
}

/* String helpers (no libc dependency) */

static bool str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++)
            return false;
    }
    return *a == *b;
}

/* Bounded str_eq: compare at most 'n' bytes, both strings must end at the same
 * position within the bound. Used for untrusted stringlists where a missing
 * NULL terminator could cause an over-read.
 */
static bool str_eq_n(const char *a, sz a_max, const char *b)
{
    while (a_max && *a && *b) {
        if (*a++ != *b++)
            return false;
        a_max--;
    }
    if (!a_max)
        return *b == '\0'; /* ran out of budget; b must also be at end */
    return *a == *b;
}

static bool str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

/* FDT header */

struct fdt_header {
    u32 magic;
    u32 totalsize;
    u32 off_dt_struct;
    u32 off_dt_strings;
    u32 off_mem_rsvmap;
    u32 version;
    u32 last_comp_version;
    u32 boot_cpuid_phys;
    u32 size_dt_strings;
    u32 size_dt_struct;
};

static struct fdt_header read_header(const void *fdt)
{
    const u8 *p = fdt;
    return (struct fdt_header) {
        .magic = fdt32(p + 0),
        .totalsize = fdt32(p + 4),
        .off_dt_struct = fdt32(p + 8),
        .off_dt_strings = fdt32(p + 12),
        .off_mem_rsvmap = fdt32(p + 16),
        .version = fdt32(p + 20),
        .last_comp_version = fdt32(p + 24),
        .boot_cpuid_phys = fdt32(p + 28),
        .size_dt_strings = fdt32(p + 32),
        .size_dt_struct = fdt32(p + 36),
    };
}

/* Read a reg property with given address/size cell counts.
 * Returns the base address via *out_base and size via *out_size.
 */
static void read_reg(const u8 *data,
                     u32 addr_cells,
                     u32 size_cells,
                     u64 *out_base,
                     u64 *out_size)
{
    if (addr_cells == 2)
        *out_base = fdt64(data);
    else
        *out_base = (u64) fdt32(data);

    const u8 *sz_data = data + (sz) addr_cells * 4;
    if (size_cells == 2)
        *out_size = fdt64(sz_data);
    else if (size_cells == 1)
        *out_size = (u64) fdt32(sz_data);
    else
        *out_size = 0;
}

/* Main parser */

/* Maximum nesting depth tracked for #address-cells / #size-cells. */
#define MAX_DEPTH 8

int fdt_parse(const void *fdt_addr, struct fdt_board_info *info)
{
    if (!fdt_addr || !info)
        return -1;

    struct fdt_header hdr = read_header(fdt_addr);
    if (hdr.magic != FDT_MAGIC) {
        printk(KERN_WARNING, STR("fdt: bad magic 0x%lx\n"), (u64) hdr.magic);
        return -1;
    }
    if (hdr.version < FDT_MIN_VERSION) {
        printk(KERN_WARNING, STR("fdt: version %lu < %lu\n"), (u64) hdr.version,
               (u64) FDT_MIN_VERSION);
        return -1;
    }

    /* Validate section offsets against totalsize so that pointers derived from
     * the header never leave the blob on malformed DTBs.
     */
    if (hdr.off_dt_struct >= hdr.totalsize ||
        hdr.off_dt_strings >= hdr.totalsize ||
        hdr.off_dt_struct + hdr.size_dt_struct > hdr.totalsize ||
        hdr.off_dt_strings + hdr.size_dt_strings > hdr.totalsize) {
        printk(KERN_WARNING, STR("fdt: section offsets exceed totalsize\n"));
        return -1;
    }

    /* Zero output */
    for (sz i = 0; i < sizeof(*info); i++)
        ((u8 *) info)[i] = 0;

    const u8 *fdt_base = (const u8 *) fdt_addr;
    const u8 *dt_struct = fdt_base + hdr.off_dt_struct;
    const u8 *dt_struct_end = dt_struct + hdr.size_dt_struct;
    const char *dt_strings = (const char *) fdt_base + hdr.off_dt_strings;
    u32 dt_strings_size = hdr.size_dt_strings;

    /* Per-depth #address-cells / #size-cells tracking.
     * Default per spec: address-cells=2, size-cells=1.
     */
    u32 addr_cells[MAX_DEPTH];
    u32 size_cells[MAX_DEPTH];
    for (int i = 0; i < MAX_DEPTH; i++) {
        addr_cells[i] = 2;
        size_cells[i] = 1;
    }

    int depth = 0;
    const u8 *p = dt_struct;
    const char *node_name;

    /* Per-node state: property order is not guaranteed, so collect reg,
     * interrupts, and riscv,ndev as we walk the node and commit them on
     * FDT_END_NODE after compatible matching has established the device type.
     */
    bool in_plic = false;
    bool in_uart = false;
    bool in_virtio = false;
    bool in_clint = false;
    bool in_memory = false;
    bool in_cpus = false;
    bool in_cpu = false;
    int in_cpu_depth = 0; /* depth at which in_cpu was set */
    bool has_reg = false; /* deferred reg data valid for this node */
    u64 node_reg_base = 0;
    u64 node_reg_size = 0;
    u32 node_interrupt = 0;
    u32 node_ndev = 0;

    while (1) {
        /* Bounds check: need at least 4 bytes for the token. */
        if (p + 4 > dt_struct_end)
            return -1;
        u32 token = fdt32(p);
        p += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            node_name = (const char *) p;
            /* Skip the null-terminated name + padding to 4-byte boundary.
             * Bound the scan so a missing NUL cannot read past the blob.
             */
            sz name_len = 0;
            while (p + name_len < dt_struct_end && p[name_len])
                name_len++;
            if (p + name_len >= dt_struct_end)
                return -1; /* no NUL found before end of struct block */
            p += (usz) ALIGN_UP(name_len + 1, 4);

            if (depth < MAX_DEPTH - 1) {
                /* Inherit parent's cell sizes. */
                addr_cells[depth + 1] = addr_cells[depth];
                size_cells[depth + 1] = size_cells[depth];
            }
            depth++;

            /* Reset per-node state (but preserve in_cpu for sub-nodes like
             * interrupt-controller inside cpu@N).
             */
            in_plic = in_uart = in_virtio = in_clint = in_memory = false;
            has_reg = false;
            node_reg_base = 0;
            node_reg_size = 0;
            node_interrupt = 0;
            node_ndev = 0;

            /* Detect /cpus node by name. */
            if (depth == 2 && str_eq(node_name, "cpus"))
                in_cpus = true;

            /* Detect cpu@ nodes inside /cpus. */
            if (in_cpus && depth == 3 && str_starts_with(node_name, "cpu@")) {
                in_cpu = true;
                in_cpu_depth = depth;
            }

            /* Detect /memory or /memory@<addr> node by name. */
            if (depth == 2 && str_starts_with(node_name, "memory") &&
                (node_name[6] == '\0' || node_name[6] == '@'))
                in_memory = true;

            break;
        }

        case FDT_END_NODE:
            /* Commit deferred property data now that the device type
             * (compatible may have appeared after reg/interrupts).
             */
            if (has_reg) {
                if (in_plic && !info->plic_base)
                    info->plic_base = node_reg_base;
                else if (in_uart && !info->uart_base)
                    info->uart_base = node_reg_base;
                else if (in_clint && !info->clint_base)
                    info->clint_base = node_reg_base;
                else if (in_virtio &&
                         info->virtio_count < FDT_MAX_VIRTIO_SLOTS) {
                    info->virtio_base[info->virtio_count] = node_reg_base;
                    info->virtio_count++;
                } else if (in_memory && !info->dram_base) {
                    info->dram_base = node_reg_base;
                    info->dram_size = node_reg_size;
                }
            }

            if (in_virtio && info->virtio_count > 0 && node_interrupt != 0)
                info->virtio_irq[info->virtio_count - 1] = node_interrupt;

            if (in_plic && node_ndev != 0)
                info->plic_nr_sources = node_ndev;

            if (in_cpu && depth == in_cpu_depth) {
                info->nr_harts++;
                in_cpu = false;
            }

            if (depth == 2 && in_cpus)
                in_cpus = false;

            if (depth <= 0)
                return -1; /* malformed DTB: more END_NODE than BEGIN_NODE */
            depth--;
            in_plic = in_uart = in_virtio = in_clint = in_memory = false;
            break;

        case FDT_PROP: {
            /* Need at least 8 bytes for prop header (len + nameoff). */
            if (p + 8 > dt_struct_end)
                return -1;
            u32 prop_len = fdt32(p);
            u32 name_offset = fdt32(p + 4);
            p += 8;
            const u8 *prop_data = p;

            /* Validate property data fits within the struct block. */
            if (p + ALIGN_UP(prop_len, 4) > dt_struct_end)
                return -1;

            /* Validate name_offset against the strings block. */
            if (name_offset >= dt_strings_size)
                break; /* skip property with invalid name */
            const char *prop_name = dt_strings + name_offset;

            /* Advance past property data, aligned to 4 bytes. */
            p += ALIGN_UP(prop_len, 4);

            /* Track #address-cells / #size-cells at current depth. */
            if (str_eq(prop_name, "#address-cells") && prop_len == 4) {
                if (depth < MAX_DEPTH)
                    addr_cells[depth] = fdt32(prop_data);
                break;
            }
            if (str_eq(prop_name, "#size-cells") && prop_len == 4) {
                if (depth < MAX_DEPTH)
                    size_cells[depth] = fdt32(prop_data);
                break;
            }

            /* Match compatible strings to identify device types. */
            if (str_eq(prop_name, "compatible")) {
                /* Walk the stringlist (multiple null-terminated strings).
                 * Use bounded str_eq_n so a missing NUL in a malformed
                 * property cannot read past prop_len.
                 */
                const char *s = (const char *) prop_data;
                const char *end = s + prop_len;
                while (s < end) {
                    sz remain = (sz) (end - s);
                    if (str_eq_n(s, remain, "riscv,plic0") ||
                        str_eq_n(s, remain, "sifive,plic-1.0.0"))
                        in_plic = true;
                    else if (str_eq_n(s, remain, "ns16550a") ||
                             str_eq_n(s, remain, "ns16550"))
                        in_uart = true;
                    else if (str_eq_n(s, remain, "virtio,mmio"))
                        in_virtio = true;
                    else if (str_eq_n(s, remain, "riscv,clint0") ||
                             str_eq_n(s, remain, "sifive,clint0"))
                        in_clint = true;
                    /* Advance past the null terminator. */
                    while (s < end && *s)
                        s++;
                    s++;
                }
                break;
            }

            /* Defer reg data, committed at FDT_END_NODE. */
            if (str_eq(prop_name, "reg")) {
                int cd = (depth < MAX_DEPTH) ? depth : MAX_DEPTH - 1;
                if (cd < 1)
                    break; /* reg at root depth, nothing to decode */
                u32 ac = addr_cells[cd - 1]; /* parent's addr_cells */
                u32 sc = size_cells[cd - 1]; /* parent's size_cells */
                /* Bounds check: prop_len must hold at least ac + sc cells. */
                if (prop_len < (ac + sc) * 4)
                    break;
                read_reg(prop_data, ac, sc, &node_reg_base, &node_reg_size);
                has_reg = true;
                break;
            }

            /* Defer riscv,ndev, committed at FDT_END_NODE. */
            if (str_eq(prop_name, "riscv,ndev") && prop_len == 4) {
                node_ndev = fdt32(prop_data);
                break;
            }

            /* Defer interrupts, committed at FDT_END_NODE. */
            if (str_eq(prop_name, "interrupts") && prop_len >= 4) {
                node_interrupt = fdt32(prop_data);
                break;
            }

            /* timebase-frequency in /cpus node. */
            if (str_eq(prop_name, "timebase-frequency") && in_cpus) {
                if (prop_len == 4)
                    info->timebase_freq = (u64) fdt32(prop_data);
                else if (prop_len == 8)
                    info->timebase_freq = fdt64(prop_data);
                break;
            }

            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            goto done;

        default:
            /* Unknown token; bail. */
            printk(KERN_WARNING,
                   STR("fdt: unknown token 0x%lx at offset %ld\n"), (u64) token,
                   (u64) (p - 4 - dt_struct));
            return -1;
        }
    }

done:
    printk(KERN_INFO,
           STR("fdt: PLIC=0x%lx UART=0x%lx CLINT=0x%lx "
               "virtio=%lu timebase=%lu harts=%lu\n"),
           info->plic_base, info->uart_base, info->clint_base,
           (u64) info->virtio_count, info->timebase_freq, (u64) info->nr_harts);

    return 0;
}
