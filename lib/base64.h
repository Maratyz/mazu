/* SPDX-License-Identifier: MIT */
#ifndef MAZU_BASE64_H
#define MAZU_BASE64_H

#include <mazu/base.h>

/* Number of bytes the encoded output will occupy (no null terminator). */
#define BASE64_ENCODED_SIZE(n) (((sz) (n) + 2) / 3 * 4)

/* Base64-encode src[0..src_len-1] into dst (no null terminator).
 * Returns the number of characters written.
 */
sz base64_encode(const u8 *src, sz src_len, char *dst);

#endif /* MAZU_BASE64_H */
