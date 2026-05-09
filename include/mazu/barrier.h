/* SPDX-License-Identifier: MIT */
/* Memory barrier taxonomy for RISC-V (RVWMO).
 *
 * Three classes:
 *   1. Device barriers  -- mb/wmb/rmb: order normal stores/loads against
 *      MMIO or DMA-coherent memory.  Always emit a fence instruction.
 *   2. SMP barriers     -- smp_mb/smp_wmb/smp_rmb: inter-CPU ordering.
 *      Emit fence on SMP builds; compile to a compiler barrier on UP.
 *   3. DMA/IO barriers  -- dma_wmb/dma_rmb: lighter memory-to-memory
 *      ordering for virtio descriptor rings.  io_wmb: RAM-to-MMIO
 *      ordering for virtio doorbell notification.
 *
 * RISC-V fence operand encoding (RVWMO §A.6):
 *   i = device input, o = device output, r = read, w = write
 *   "fence rw,rw" = full barrier
 *   "fence w,w"   = write-write (store barrier)
 *   "fence r,r"   = read-read (load barrier)
 *   "fence w,o"   = RAM write before MMIO store (doorbell pattern)
 *   "fence iorw,iorw" = full device barrier (includes MMIO)
 */

#ifndef MAZU_BARRIER_H
#define MAZU_BARRIER_H

/* Compiler barrier -- prevents reordering across this point but emits
 * no hardware fence.  Sufficient for UP inter-"CPU" ordering.
 */
#define barrier() __asm__ volatile("" ::: "memory")

/*
 * Device / DMA barriers -- always emit a fence regardless of SMP config.
 */

/* Full device barrier (normal + MMIO). */
static inline void mb(void)
{
    __asm__ volatile("fence iorw,iorw" ::: "memory");
}

/* Device write barrier. */
static inline void wmb(void)
{
    __asm__ volatile("fence ow,ow" ::: "memory");
}

/* Device read barrier. */
static inline void rmb(void)
{
    __asm__ volatile("fence ir,ir" ::: "memory");
}

/*
 * SMP barriers -- inter-CPU ordering.
 * On UP builds these reduce to compiler barriers (zero fence instructions).
 */

#if CONFIG_SMP

static inline void smp_mb(void)
{
    __asm__ volatile("fence rw,rw" ::: "memory");
}

static inline void smp_wmb(void)
{
    __asm__ volatile("fence w,w" ::: "memory");
}

static inline void smp_rmb(void)
{
    __asm__ volatile("fence r,r" ::: "memory");
}

#else /* !CONFIG_SMP */

static inline void smp_mb(void)
{
    barrier();
}

static inline void smp_wmb(void)
{
    barrier();
}

static inline void smp_rmb(void)
{
    barrier();
}

#endif /* CONFIG_SMP */

/*
 * DMA barriers -- lighter memory-to-memory ordering for descriptor rings.
 * These order normal memory accesses against each other (not MMIO).
 */

/* DMA write barrier: descriptor writes visible before bumping avail->idx. */
static inline void dma_wmb(void)
{
    __asm__ volatile("fence w,w" ::: "memory");
}

/* DMA read barrier: used->idx read ordered before used_ring element reads. */
static inline void dma_rmb(void)
{
    __asm__ volatile("fence r,r" ::: "memory");
}

/* IO write barrier: RAM writes visible before MMIO doorbell store.
 * Distinct from dma_wmb -- this orders memory writes before a device
 * output store (the virtio MMIO notify register).
 */
static inline void io_wmb(void)
{
    __asm__ volatile("fence w,o" ::: "memory");
}

#endif /* MAZU_BARRIER_H */
