/* SPDX-License-Identifier: MIT */
#include <mazu/net/udp.h>
#include <mazu/selftest.h>

static struct result dummy_handler(struct netdev *d __unused,
                                   struct ipv4_addr s __unused,
                                   u16 sp __unused,
                                   u16 dp __unused,
                                   struct byte_view p __unused,
                                   struct send_buf sb __unused,
                                   struct arena t __unused)
{
    return result_ok();
}

static i32 test_udp_register(void)
{
    /* Save and clear the table so tests start from a known state. */
    struct udp_listener saved[UDP_MAX_LISTENERS];
    sz saved_count = udp_listener_count;
    for (sz i = 0; i < udp_listener_count; i++)
        saved[i] = udp_listeners[i];
    udp_listener_count = 0;

    /* Register a listener. */
    struct result r = udp_register(1234, dummy_handler);
    if (r.is_error)
        return 1;
    if (udp_listener_count != 1)
        return 2;

    /* Duplicate detection. */
    r = udp_register(1234, dummy_handler);
    if (!r.is_error || r.code != EADDRINUSE)
        return 3;

    /* Unregister. */
    udp_unregister(1234);
    if (udp_listener_count != 0)
        return 4;

    /* Re-register after unregister. */
    r = udp_register(1234, dummy_handler);
    if (r.is_error)
        return 5;

    /* Fill the table. */
    udp_unregister(1234);
    for (u16 p = 0; p < UDP_MAX_LISTENERS; p++) {
        r = udp_register((u16) (5000 + p), dummy_handler);
        if (r.is_error)
            return 10 + p;
    }

    /* Table full. */
    r = udp_register(9999, dummy_handler);
    if (!r.is_error || r.code != ENOMEM)
        return 20;

    /* Restore original table. */
    udp_listener_count = saved_count;
    for (sz i = 0; i < saved_count; i++)
        udp_listeners[i] = saved[i];

    return 0;
}

DEFINE_SELFTEST(udp_register, test_udp_register);
