/* SPDX-License-Identifier: MIT */
#include <kernel/kres.h>
#include <mazu/ipi.h>
#include <mazu/selftest.h>

/* Test 1: register + unregister.  Destructor should NOT be called. */
static volatile bool kres_test_destroyed;

struct kres_test_obj {
    struct kres kr;
    int id;
};

static void kres_test_destroy(struct kres *kr __unused)
{
    kres_test_destroyed = true;
}

static i32 test_kres_register_unregister(void)
{
    kres_test_destroyed = false;

    struct kres_test_obj obj;
    kres_init(&obj.kr);
    kres_register(&obj.kr, kres_test_destroy);
    kres_unregister(&obj.kr);

    /* Destructor should NOT have been called. */
    if (kres_test_destroyed)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kres_register_unregister, test_kres_register_unregister);

/* Test 2: auto-cleanup on task exit.  Task registers a kres then exits;
 * destructor should fire during deferred-free.
 */
static volatile bool kres_auto_destroyed;
static struct kres_test_obj kres_auto_obj;

static void kres_auto_destroy(struct kres *kr __unused)
{
    kres_auto_destroyed = true;
}

static void kres_auto_task(void *ctx __unused)
{
    kres_init(&kres_auto_obj.kr);
    kres_register(&kres_auto_obj.kr, kres_auto_destroy);
    /* Task returns -> sched_task_finish -> TERMINATING -> deferred free
     * -> kres_destroy_all.
     */
}

static i32 test_kres_auto_cleanup(void)
{
    kres_auto_destroyed = false;

    struct result r = sched_create_task(kres_auto_task, NULL);
    if (r.is_error)
        return 1;

    /* Let the task run and exit. */
    sleep_ms(time_ms_new(50));
    /* Kick all remote harts so they run sched_schedule() and execute the
     * deferred-free path.  Under tickless idle, a remote hart may sit in
     * WFI indefinitely without this IPI.
     */
    ipi_send_broadcast(IPI_SCHED);
    sleep_ms(time_ms_new(50));

    if (!kres_auto_destroyed)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kres_auto_cleanup, test_kres_auto_cleanup);

/* Test 3: multiple kres entries destroyed in LIFO order. */
static volatile int kres_multi_order[3];
static volatile int kres_multi_idx;

struct kres_multi_obj {
    struct kres kr;
    int id;
};

static struct kres_multi_obj kres_multi_objs[3];

static void kres_multi_destroy(struct kres *kr)
{
    struct kres_multi_obj *obj = (struct kres_multi_obj *) kr;
    kres_multi_order[kres_multi_idx++] = obj->id;
}

static void kres_multi_task(void *ctx __unused)
{
    for (int i = 0; i < 3; i++) {
        kres_multi_objs[i].id = i;
        kres_init(&kres_multi_objs[i].kr);
        kres_register(&kres_multi_objs[i].kr, kres_multi_destroy);
    }
    /* Task exits -> kres_destroy_all in LIFO order: 2, 1, 0. */
}

static i32 test_kres_multiple(void)
{
    kres_multi_idx = 0;
    kres_multi_order[0] = kres_multi_order[1] = kres_multi_order[2] = -1;

    struct result r = sched_create_task(kres_multi_task, NULL);
    if (r.is_error)
        return 1;

    sleep_ms(time_ms_new(50));
    ipi_send_broadcast(IPI_SCHED);
    sleep_ms(time_ms_new(50));

    /* LIFO: registered 0, 1, 2 -> destroyed 2, 1, 0. */
    if (kres_multi_order[0] != 2 || kres_multi_order[1] != 1 ||
        kres_multi_order[2] != 0)
        return 1;
    return 0;
}
DEFINE_SELFTEST(kres_multiple, test_kres_multiple);
