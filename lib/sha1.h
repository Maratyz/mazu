/* SPDX-License-Identifier: MIT */
#ifndef MAZU_SHA1_H
#define MAZU_SHA1_H

#include <mazu/base.h>

#define SHA1_DIGEST_SIZE 20

/* Compute SHA-1 digest of 'data[0..len-1]' and write 20 bytes to 'out'. */
void sha1(const u8 *data, sz len, u8 out[SHA1_DIGEST_SIZE]);

#endif /* MAZU_SHA1_H */
