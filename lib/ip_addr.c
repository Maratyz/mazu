/* SPDX-License-Identifier: MIT */
#include <mazu/net/ip_addr.h>

/* Parse a bounded decimal integer from str at position *pos.
 * At most 'max_digits' characters are consumed (prevents overflow).
 * No leading zeros allowed (e.g. "07" stops after '0').
 * Returns the parsed value, or -1 on failure.
 * Advances *pos past the consumed digits.
 */
static sz parse_decimal(struct str str, sz *pos, sz max_digits)
{
    sz p = *pos;
    if (p >= str.len || str.dat[p] < '0' || str.dat[p] > '9')
        return -1;

    sz val = str.dat[p] - '0';
    p++;

    if (val == 0) {
        /* '0' alone is valid; '0x' where x is a digit means leading zero,
         * so stop here and let the caller reject the leftover digit.
         */
        *pos = p;
        return 0;
    }

    sz digits = 1;
    while (digits < max_digits && p < str.len && str.dat[p] >= '0' &&
           str.dat[p] <= '9') {
        val = val * 10 + (str.dat[p] - '0');
        p++;
        digits++;
    }

    *pos = p;
    return val;
}

static struct ipv4_addr prefix_length_to_mask(sz prefix_length)
{
    assert(1 <= prefix_length && prefix_length <= 32);

    u32 raw = 0xffffffff << (32 - prefix_length);

    return (struct ipv4_addr) {
        .addr = {(raw >> 24) & 0xff, (raw >> 16) & 0xff, (raw >> 8) & 0xff,
                 raw & 0xff},
    };
}

struct result_ipv4_addr_parsed ipv4_addr_parse(struct str str)
{
    struct ipv4_addr_parsed pa = {
        .addr = IPV4_ADDR_ANY,
        .mask = IPV4_ADDR_ANY,
    };

    sz i = 0;

    /* Parse four dot-separated octets: d.d.d.d */
    for (sz idx = 0; idx < 4; idx++) {
        sz val = parse_decimal(str, &i, 3);
        if (val < 0 || val > 255)
            goto error;
        pa.addr.addr[idx] = (u8) val;

        if (idx < 3) {
            if (i >= str.len || str.dat[i] != '.')
                goto error;
            i++;
        }
    }

    /* Parse optional CIDR prefix: /p where p is 1-32. */
    sz prefix_length = 32;
    if (i < str.len) {
        if (str.dat[i] != '/')
            goto error;
        i++;

        prefix_length = parse_decimal(str, &i, 2);
        if (prefix_length < 1 || prefix_length > 32)
            goto error;
    }

    if (i != str.len)
        goto error;

    pa.mask = prefix_length_to_mask(prefix_length);

    return result_ipv4_addr_parsed_ok(pa);

error:
    return result_ipv4_addr_parsed_error(EINVAL);
}

#include __INC_TEST(ip_addr)
