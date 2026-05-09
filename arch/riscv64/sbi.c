/* SPDX-License-Identifier: MIT */
/* SBI ecall implementation.
 *
 * Provides the low-level ecall wrapper and typed wrappers for each SBI
 * extension used by Mazu.  sbi_init() probes available extensions and
 * logs their availability.
 */

#include <mazu/base.h>
#include <mazu/cpumask.h>
#include <mazu/pcpu.h>
#include <mazu/print.h>
#include <sbi.h>

/* UP critical-section nesting state (compiled out when CONFIG_SMP=y,
 * since the SMP version lives in struct pcpu).
 */
#if !CONFIG_SMP
u32 _up_critnest;
u64 _up_saved_sie;

/* UP pcpu array and interrupt stacks, single hart only. */
struct pcpu pcpu_array[MAX_CPUS] __aligned(64);
char intr_stacks[MAX_CPUS][INTR_STACK_SIZE] __aligned(16);
volatile u32 nr_cpus_online = 1;
#endif

/* Global CPU state masks -- used by cpumask.h inline helpers. */
struct mp_state mp_state;

bool sbi_has_hsm;

struct sbiret sbi_ecall(u64 eid, u64 fid, u64 a0, u64 a1, u64 a2)
{
    register u64 r_a0 asm("a0") = a0;
    register u64 r_a1 asm("a1") = a1;
    register u64 r_a2 asm("a2") = a2;
    register u64 r_a6 asm("a6") = fid;
    register u64 r_a7 asm("a7") = eid;

    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a6), "r"(r_a7)
                     : "memory");

    return (struct sbiret) {.error = (i64) r_a0, .value = (i64) r_a1};
}

static bool sbi_probe_extension(u64 eid)
{
    struct sbiret ret =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_PROBE_EXTENSION, eid, 0, 0);
    return ret.error == SBI_SUCCESS && ret.value != 0;
}

void sbi_init(void)
{
    /* Initialize BSP's pcpu entry (always, even on UP). */
#if !CONFIG_SMP
    pcpu_array[0].hartid = 0;
#endif
    pcpu_array[0].cpuid = 0;
    pcpu_array[0].intr_stack_top = (u64 *) &intr_stacks[0][INTR_STACK_SIZE];
    pcpu_array[0].online = true;
    mp_set_cpu_active(0);

    /* Query SBI spec version for logging. */
    struct sbiret ver =
        sbi_ecall(SBI_EXT_BASE, SBI_BASE_GET_SPEC_VERSION, 0, 0, 0);
    u64 spec_major = ((u64) ver.value >> 24) & 0x7f;
    u64 spec_minor = (u64) ver.value & 0xffffff;

    sbi_has_hsm = sbi_probe_extension(SBI_EXT_HSM);
    bool has_ipi = sbi_probe_extension(SBI_EXT_IPI);
    bool has_timer = sbi_probe_extension(SBI_EXT_TIMER);
    printk(KERN_INFO, STR("SBI v%lu.%lu: HSM=%d IPI=%d Timer=%d\n"), spec_major,
           spec_minor, (int) sbi_has_hsm, (int) has_ipi, (int) has_timer);
}

/* HSM extension */
struct sbiret sbi_hart_start(u64 hartid, u64 start_addr, u64 opaque)
{
    return sbi_ecall(SBI_EXT_HSM, SBI_HSM_HART_START, hartid, start_addr,
                     opaque);
}

/* IPI extension */
struct sbiret sbi_send_ipi(u64 hart_mask, u64 hart_mask_base)
{
    return sbi_ecall(SBI_EXT_IPI, SBI_IPI_SEND_IPI, hart_mask, hart_mask_base,
                     0);
}

/* Timer extension */
void sbi_set_timer(u64 stime_value)
{
    sbi_ecall(SBI_EXT_TIMER, SBI_TIMER_SET_TIMER, stime_value, 0, 0);
}

#if CONFIG_SEMIHOSTING
#include __INC_TEST(sbi)
#endif /* CONFIG_SEMIHOSTING */
