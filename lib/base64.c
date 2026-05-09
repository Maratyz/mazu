/* SPDX-License-Identifier: MIT */
/* Minimal Base64 encoder.
 *
 * Only encodes (no decoding needed for WebSocket).
 * Output does NOT include a null terminator - caller sizes the buffer with
 * BASE64_ENCODED_SIZE(input_len).
 */

#include "base64.h"

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
    "uvwxyz0123456789+/";

sz base64_encode(const u8 *src, sz src_len, char *dst)
{
    sz i = 0, j = 0;

    while (i + 2 < src_len) {
        dst[j++] = b64_table[(src[i] >> 2) & 0x3f];
        dst[j++] = b64_table[((src[i] & 0x03) << 4) | (src[i + 1] >> 4)];
        dst[j++] = b64_table[((src[i + 1] & 0x0f) << 2) | (src[i + 2] >> 6)];
        dst[j++] = b64_table[src[i + 2] & 0x3f];
        i += 3;
    }

    if (i < src_len) {
        dst[j++] = b64_table[(src[i] >> 2) & 0x3f];
        if (i + 1 < src_len) {
            dst[j++] = b64_table[((src[i] & 0x03) << 4) | (src[i + 1] >> 4)];
            dst[j++] = b64_table[(src[i + 1] & 0x0f) << 2];
        } else {
            dst[j++] = b64_table[(src[i] & 0x03) << 4];
            dst[j++] = '=';
        }
        dst[j++] = '=';
    }

    return j;
}
