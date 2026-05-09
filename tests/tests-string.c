/* SPDX-License-Identifier: MIT */
#include <mazu/base.h>
#include <mazu/selftest.h>

static i32 test_memcpy_memset(void)
{
    byte src[32], dst[32];

    /* Fill src with a pattern, clear dst. */
    for (sz i = 0; i < (sz) sizeof(src); i++)
        src[i] = (byte) (i ^ 0xA5);
    memset(dst, 0, sizeof(dst));

    /* Copy and verify. */
    memcpy(dst, src, sizeof(src));
    for (sz i = 0; i < (sz) sizeof(src); i++) {
        if (dst[i] != src[i])
            return 1;
    }

    /* Memset and verify. */
    memset(dst, 0x42, sizeof(dst));
    for (sz i = 0; i < (sz) sizeof(dst); i++) {
        if (dst[i] != 0x42)
            return 1;
    }

    return 0;
}

DEFINE_SELFTEST(memcpy_memset, test_memcpy_memset);
