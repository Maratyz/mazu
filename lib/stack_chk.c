/* SPDX-License-Identifier: MIT */
/* Stack-protector support for GCC -fstack-protector-strong.
 *
 * __stack_chk_guard is the canary value placed between local variables and
 * the return address.  __stack_chk_fail() is called when the canary is
 * corrupted, indicating a stack buffer overflow.
 *
 * The guard is initialized early in boot from entropy-mixed rdtime() values
 * to resist predictable-boot attacks.  See kernel/init/main.c.
 */

#include <mazu/base.h>
#include <mazu/print.h>

/* The canary value.  Must be initialized before any function with
 * stack-protector instrumentation is called.  Set in stack_chk_init().
 */
u64 __stack_chk_guard;

/* Called by GCC-generated code when the stack canary is corrupted. */
__attribute__((noreturn)) void __stack_chk_fail(void)
{
    printk(KERN_ERR, STR("*** STACK SMASHING DETECTED ***\n"));
    printk(KERN_ERR, STR("Stack buffer overflow: canary corrupted, "
                         "halting.\n"));

    /* Hard halt: do not attempt cleanup as the stack may be corrupted. */
    __asm__ volatile("csrci sstatus, 2"); /* disable interrupts */
    while (true)
        __asm__ volatile("wfi");
    __builtin_unreachable();
}

/* Initialize the stack canary from multiple entropy sources.
 * Called very early in boot before any stack-protected function runs.
 *
 * Must NOT be instrumented by -fstack-protector: the prologue would read
 * the uninitialized __stack_chk_guard (zero), the body sets it to a
 * non-zero value, and the epilogue would detect a mismatch - guaranteed
 * boot panic.
 */
__attribute__((no_stack_protector)) void stack_chk_init(void)
{
    u64 t1, t2;
    __asm__ volatile("csrr %0, time" : "=r"(t1));
    /* Small delay to get different time value. */
    for (volatile int i = 0; i < 100; i++)
        ;
    __asm__ volatile("csrr %0, time" : "=r"(t2));

    /* Read hart ID for per-hart differentiation. */
    u64 hart;
    __asm__ volatile("csrr %0, sscratch" : "=r"(hart));

    /* Mix multiple sources: two time samples, hart ID, and FDT magic.
     * Fold via xorshift to spread entropy across all bits.
     */
    u64 guard = t1 ^ (t2 << 17) ^ (hart * 0x9E3779B97F4A7C15ULL);
    guard ^= guard >> 33;
    guard *= 0xFF51AFD7ED558CCDULL;
    guard ^= guard >> 33;
    guard *= 0xC4CEB9FE1A85EC53ULL;
    guard ^= guard >> 33;

    /* Ensure the guard is never zero (zero canaries are trivially
     * predictable and some GCC versions treat zero as uninitialized).
     */
    if (guard == 0)
        guard = 0xDEADBEEFCAFEBABEULL;

    __stack_chk_guard = guard;
}

#if CONFIG_SEMIHOSTING
#include __INC_TEST(stack_chk)
#endif
