/* SPDX-License-Identifier: MIT */
#include <mazu/kvalloc.h>
#include <mazu/net/ip_addr.h>
#include <mazu/selftest.h>

static i32 test_ip_addr_parse(void)
{
    struct byte_array ba = option_byte_array_checked(kvalloc_alloc(0x2000, 64));
    struct arena arn = arena_new(ba);

    struct result_ipv4_addr_parsed pa_res;
    struct ipv4_addr_parsed pa;

    /* Valid addresses */
    pa_res = ipv4_addr_parse(STR("0.0.0.0"));
    assert(!pa_res.is_error);
    pa = result_ipv4_addr_parsed_checked(pa_res);
    assert(pa.addr.addr[0] == 0 && pa.addr.addr[1] == 0 &&
           pa.addr.addr[2] == 0 && pa.addr.addr[3] == 0);
    assert(pa.mask.addr[0] == 0xff && pa.mask.addr[1] == 0xff &&
           pa.mask.addr[2] == 0xff && pa.mask.addr[3] == 0xff);

    pa_res = ipv4_addr_parse(STR("255.255.255.255"));
    assert(!pa_res.is_error);
    pa = result_ipv4_addr_parsed_checked(pa_res);
    assert(pa.addr.addr[0] == 255 && pa.addr.addr[1] == 255 &&
           pa.addr.addr[2] == 255 && pa.addr.addr[3] == 255);
    assert(pa.mask.addr[0] == 0xff && pa.mask.addr[1] == 0xff &&
           pa.mask.addr[2] == 0xff && pa.mask.addr[3] == 0xff);

    pa_res = ipv4_addr_parse(STR("1.23.195.7"));
    assert(!pa_res.is_error);
    pa = result_ipv4_addr_parsed_checked(pa_res);
    assert(pa.addr.addr[0] == 1 && pa.addr.addr[1] == 23 &&
           pa.addr.addr[2] == 195 && pa.addr.addr[3] == 7);
    assert(pa.mask.addr[0] == 0xff && pa.mask.addr[1] == 0xff &&
           pa.mask.addr[2] == 0xff && pa.mask.addr[3] == 0xff);

    pa_res = ipv4_addr_parse(STR("127.42.8.100"));
    assert(!pa_res.is_error);
    pa = result_ipv4_addr_parsed_checked(pa_res);
    assert(pa.addr.addr[0] == 127 && pa.addr.addr[1] == 42 &&
           pa.addr.addr[2] == 8 && pa.addr.addr[3] == 100);
    assert(pa.mask.addr[0] == 0xff && pa.mask.addr[1] == 0xff &&
           pa.mask.addr[2] == 0xff && pa.mask.addr[3] == 0xff);

    /* Invalid addresses */
    pa_res = ipv4_addr_parse(STR("256.0.0.0"));
    assert(pa_res.is_error);

    pa_res = ipv4_addr_parse(STR("192.168.1"));
    assert(pa_res.is_error);

    pa_res = ipv4_addr_parse(STR("001.002.003.004"));
    assert(pa_res.is_error);

    /* CIDR tests */
    struct str_buf sbuf =
        str_buf_from_byte_array(byte_array_from_arena(32, &arn));
    for (sz prefix = 1; prefix <= 32; prefix++) {
        sbuf.len = 0;
        str_buf_append(&sbuf, STR("192.168.0.1/"));
        fmt_append_i64(prefix, &sbuf);

        pa_res = ipv4_addr_parse(str_from_buf(sbuf));
        assert(!pa_res.is_error);
        pa = result_ipv4_addr_parsed_checked(pa_res);
        assert(pa.addr.addr[0] == 192 && pa.addr.addr[1] == 168 &&
               pa.addr.addr[2] == 0 && pa.addr.addr[3] == 1);

        u32 mask_raw = 0xffffffff << (32 - prefix);
        assert(pa.mask.addr[0] == ((mask_raw >> 24) & 0xff));
        assert(pa.mask.addr[1] == ((mask_raw >> 16) & 0xff));
        assert(pa.mask.addr[2] == ((mask_raw >> 8) & 0xff));
        assert(pa.mask.addr[3] == (mask_raw & 0xff));
    }

    kvalloc_free(ba);
    return 0;
}
DEFINE_SELFTEST(ip_addr_parse, test_ip_addr_parse);
