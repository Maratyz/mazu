/* SPDX-License-Identifier: MIT */
/* Host-side stub for mazu/barrier.h -- all barriers are compiler-only. */
#ifndef MAZU_BARRIER_H
#define MAZU_BARRIER_H

#define barrier() __asm__ volatile("" ::: "memory")
#define mb() __asm__ volatile("" ::: "memory")
#define wmb() __asm__ volatile("" ::: "memory")
#define rmb() __asm__ volatile("" ::: "memory")
#define smp_mb() __asm__ volatile("" ::: "memory")
#define smp_wmb() __asm__ volatile("" ::: "memory")
#define smp_rmb() __asm__ volatile("" ::: "memory")
#define dma_wmb() __asm__ volatile("" ::: "memory")
#define dma_rmb() __asm__ volatile("" ::: "memory")
#define io_wmb() __asm__ volatile("" ::: "memory")

#endif /* MAZU_BARRIER_H */
