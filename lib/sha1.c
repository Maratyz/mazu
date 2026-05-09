/* SPDX-License-Identifier: MIT */
/* Minimal SHA-1 implementation.
 *
 * Based on Steve Reid's public-domain SHA-1 (sha1.c, 1995).
 * Adapted for the Mazu kernel (no stdlib, project types).
 */

#include "sha1.h"

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define SHA1_F1(b, c, d) (((b) & (c)) | ((~(b)) & (d)))
#define SHA1_F2(b, c, d) ((b) ^ (c) ^ (d))
#define SHA1_F3(b, c, d) (((b) & (c)) | ((b) & (d)) | ((c) & (d)))

#define SHA1_K1 0x5A827999UL
#define SHA1_K2 0x6ED9EBA1UL
#define SHA1_K3 0x8F1BBCDCUL
#define SHA1_K4 0xCA62C1D6UL

static void sha1_transform(u32 state[5], const u8 block[64])
{
    u32 w[80];
    u32 a, b, c, d, e, tmp;

    /* Expand block into schedule. */
    for (sz i = 0; i < 16; i++) {
        w[i] = ((u32) block[i * 4 + 0] << 24) | ((u32) block[i * 4 + 1] << 16) |
               ((u32) block[i * 4 + 2] << 8) | ((u32) block[i * 4 + 3]);
    }
    for (sz i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

#define ROUND(f, k, i)                              \
    do {                                            \
        tmp = ROL32(a, 5) + (f) + e + (k) + w[(i)]; \
        e = d;                                      \
        d = c;                                      \
        c = ROL32(b, 30);                           \
        b = a;                                      \
        a = tmp;                                    \
    } while (0)

    for (sz i = 0; i < 20; i++)
        ROUND(SHA1_F1(b, c, d), SHA1_K1, i);
    for (sz i = 20; i < 40; i++)
        ROUND(SHA1_F2(b, c, d), SHA1_K2, i);
    for (sz i = 40; i < 60; i++)
        ROUND(SHA1_F3(b, c, d), SHA1_K3, i);
    for (sz i = 60; i < 80; i++)
        ROUND(SHA1_F2(b, c, d), SHA1_K4, i);

#undef ROUND

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1(const u8 *data, sz len, u8 out[SHA1_DIGEST_SIZE])
{
    u32 state[5] = {
        0x67452301UL, 0xEFCDAB89UL, 0x98BADCFEUL, 0x10325476UL, 0xC3D2E1F0UL,
    };

    /* Process full 64-byte blocks. */
    sz n_full = len / 64;
    for (sz i = 0; i < n_full; i++)
        sha1_transform(state, data + i * 64);

    /* Build the padded final block(s). */
    sz remain = len % 64;
    u8 pad[128];
    for (sz i = 0; i < remain; i++)
        pad[i] = data[n_full * 64 + i];
    pad[remain] = 0x80;
    sz pad_len = (remain < 56) ? 64 : 128;
    for (sz i = remain + 1; i < pad_len - 8; i++)
        pad[i] = 0;

    /* Append bit-length (big-endian 64-bit). */
    u64 bit_len = (u64) len * 8;
    for (sz i = 0; i < 8; i++)
        pad[pad_len - 8 + i] = (u8) (bit_len >> (56 - i * 8));

    sha1_transform(state, pad);
    if (pad_len == 128)
        sha1_transform(state, pad + 64);

    /* Write big-endian output. */
    for (sz i = 0; i < 5; i++) {
        out[i * 4 + 0] = (u8) (state[i] >> 24);
        out[i * 4 + 1] = (u8) (state[i] >> 16);
        out[i * 4 + 2] = (u8) (state[i] >> 8);
        out[i * 4 + 3] = (u8) (state[i]);
    }
}
