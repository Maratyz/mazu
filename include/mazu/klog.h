/* SPDX-License-Identifier: MIT */
/* Kernel log ring buffer.
 *
 * IRQ-safe write path (irqsave spinlock).  Drain kthread consumes the
 * ring to UART.  After klog_init, printk output goes through the ring
 * instead of directly to UART, decoupling console I/O from task latency.
 *
 * Overflow: oldest data is dropped, klog_dropped() tracks lost bytes.
 */

#ifndef MAZU_KLOG_H
#define MAZU_KLOG_H

#include <mazu/base.h>

#ifndef CONFIG_KLOG_SIZE
#define CONFIG_KLOG_SIZE 4096
#endif
#define KLOG_SIZE CONFIG_KLOG_SIZE /* must be power of 2 */

void klog_init(void);
void klog_write(const char *msg, sz len);
void klog_fault_event(const char *kind,
                      u32 cpu,
                      u16 pid,
                      u16 tid,
                      u64 scause,
                      u64 sepc,
                      u64 stval,
                      u64 sstatus);

/* Security event: log a denied syscall or policy violation. */
void klog_security_event(const char *action,
                         u16 pid,
                         u16 tid,
                         u64 syscall_nr,
                         u16 err_code);
sz klog_drain(char *out, sz max);
sz klog_peek(char *out, sz max);
bool klog_has_data(void);
u64 klog_dropped(void);

#endif /* MAZU_KLOG_H */
