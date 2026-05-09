/* SPDX-License-Identifier: MIT */
/* SBI (Supervisor Binary Interface) abstraction layer.
 *
 * Wraps the SBI ecall mechanism defined by the RISC-V SBI specification. Probes
 * extension availability at boot and provides typed wrappers for HSM, IPI, and
 * Timer extensions.
 */

#ifndef MAZU_SBI_H
#define MAZU_SBI_H

#include <mazu/base.h>

/* SBI return value pair. */
struct sbiret {
    i64 error;
    i64 value;
};

/* SBI error codes. */
#define SBI_SUCCESS 0
#define SBI_ERR_FAILED (-1)
#define SBI_ERR_NOT_SUPPORTED (-2)
#define SBI_ERR_INVALID_PARAM (-3)
#define SBI_ERR_DENIED (-4)
#define SBI_ERR_INVALID_ADDRESS (-5)
#define SBI_ERR_ALREADY_AVAILABLE (-6)

/* Extension IDs. */
#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIMER 0x54494D45 /* TIME */
#define SBI_EXT_IPI 0x735049     /* sPI  */
#define SBI_EXT_HSM 0x48534D     /* HSM  */

/* Base extension function IDs. */
#define SBI_BASE_GET_SPEC_VERSION 0
#define SBI_BASE_PROBE_EXTENSION 3

/* HSM function IDs. */
#define SBI_HSM_HART_START 0
#define SBI_HSM_HART_STOP 1

/* IPI function IDs. */
#define SBI_IPI_SEND_IPI 0

/* Timer function IDs. */
#define SBI_TIMER_SET_TIMER 0

/* Low-level ecall wrapper. */
struct sbiret sbi_ecall(u64 eid, u64 fid, u64 a0, u64 a1, u64 a2);

/* Capability flags (set by sbi_init). */
extern bool sbi_has_hsm;
/* Initialize SBI: probe extensions, log capabilities. */
void sbi_init(void);

/* HSM extension. */
struct sbiret sbi_hart_start(u64 hartid, u64 start_addr, u64 opaque);

/* IPI extension. */
struct sbiret sbi_send_ipi(u64 hart_mask, u64 hart_mask_base);

/* Timer extension. */
void sbi_set_timer(u64 stime_value);

#endif /* MAZU_SBI_H */
