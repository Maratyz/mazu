/* SPDX-License-Identifier: MIT */
/* Minimal C string/memory functions.
 *
 * On x86-64 GCC can inline struct copies using REP MOVSB; on RISC-V it
 * emits calls to memcpy/memmove/memset.  Provide simple implementations so
 * the bare-metal build links cleanly on both architectures.
 */

#include <mazu/base.h>

void *memcpy(void *dest, const void *src, sz n)
{
    byte *d = (byte *) dest;
    const byte *s = (const byte *) src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, sz n)
{
    byte *d = (byte *) dest;
    const byte *s = (const byte *) src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

void *memset(void *dest, int c, sz n)
{
    byte *d = (byte *) dest;
    while (n--)
        *d++ = (byte) c;
    return dest;
}

#if CONFIG_SEMIHOSTING
#include __INC_TEST(string)
#endif /* CONFIG_SEMIHOSTING */
