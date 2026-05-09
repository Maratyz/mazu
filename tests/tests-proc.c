/* SPDX-License-Identifier: MIT */
#include <mazu/proc.h>
#include <mazu/selftest.h>

static i32 selftest_proc_alloc_free(void)
{
    struct proc *p = proc_alloc();
    assert(p != NULL);
    assert(p->magic == PROC_MAGIC);
    assert(p->state == PROC_STATE_EMBRYO);
    assert(p->pid > 0);
    u16 pid = p->pid;
    u32 gen_before = p->generation;
    assert(proc_find(pid) == p);
    proc_free(p);
    assert(p->state == PROC_STATE_FREE);
    assert(p->magic == 0xDEADDEADU);         /* poisoned after free */
    assert(p->generation == gen_before + 1); /* generation incremented */
    assert(proc_find(pid) == NULL);
    return 0;
}
DEFINE_SELFTEST(proc_alloc_free, selftest_proc_alloc_free);

static i32 selftest_proc_fd_table(void)
{
    struct proc *p = proc_alloc();
    assert(p != NULL);
    assert(p->fd_table[PROC_FD_STDIN].is_open);
    assert(p->fd_table[PROC_FD_STDOUT].is_open);
    assert(p->fd_table[PROC_FD_STDERR].is_open);
    for (sz i = PROC_FD_STDERR + 1; i < PROC_FD_MAX; i++)
        assert(!p->fd_table[i].is_open);
    proc_free(p);
    return 0;
}
DEFINE_SELFTEST(proc_fd_table, selftest_proc_fd_table);

static i32 selftest_proc_pid_wrap_collision(void)
{
    u16 old_next_pid = next_pid;
    next_pid = U16_MAX;

    struct proc *p1 = proc_alloc();
    struct proc *p2 = proc_alloc();
    assert(p1 != NULL);
    assert(p2 != NULL);
    assert(p1->pid == U16_MAX);
    assert(p2->pid == 1);
    assert(p1->pid != p2->pid);

    proc_free(p1);
    proc_free(p2);
    next_pid = old_next_pid;
    return 0;
}
DEFINE_SELFTEST(proc_pid_wrap_collision, selftest_proc_pid_wrap_collision);

static i32 selftest_proc_vma_tracking(void)
{
    struct proc *p = proc_alloc();
    assert(p != NULL);
    assert(p->n_vmas == 0);

    /* Register a code VMA at USER_CODE_BASE, 2 pages. */
    i32 rc = proc_add_vma(p, USER_CODE_BASE, 2UL * PAGE_SIZE,
                          VMA_PERM_READ | VMA_PERM_EXEC);
    assert(rc == 0);
    assert(p->n_vmas == 1);

    /* Register a stack VMA. */
    rc = proc_add_vma(p, (ptr) (USER_STACK_TOP - 4UL * PAGE_SIZE),
                      4UL * PAGE_SIZE, VMA_PERM_READ | VMA_PERM_WRITE);
    assert(rc == 0);
    assert(p->n_vmas == 2);

    /* Address inside code VMA should be found. */
    assert(proc_vma_contains(p, USER_CODE_BASE + 0x100, 0x10));

    /* Address inside stack VMA should be found. */
    assert(proc_vma_contains(p, (ptr) (USER_STACK_TOP - PAGE_SIZE), 0x10));

    /* Address outside any VMA should NOT be found. */
    assert(!proc_vma_contains(p, USER_DATA_BASE, 0x10));

    /* Zero-length access is always valid. */
    assert(proc_vma_contains(p, 0, 0));

    /* Cross-VMA boundary should fail. */
    assert(!proc_vma_contains(p, USER_CODE_BASE + PAGE_SIZE, 2UL * PAGE_SIZE));

    proc_free(p);
    return 0;
}
DEFINE_SELFTEST(proc_vma_tracking, selftest_proc_vma_tracking);

static i32 selftest_proc_state_machine(void)
{
    struct proc *p = proc_alloc();
    assert(p != NULL);
    assert(p->state == PROC_STATE_EMBRYO);

    /* EMBRYO -> RUNNING */
    proc_set_state(p, PROC_STATE_RUNNING);
    assert(p->state == PROC_STATE_RUNNING);

    /* RUNNING -> ZOMBIE */
    proc_set_state(p, PROC_STATE_ZOMBIE);
    assert(p->state == PROC_STATE_ZOMBIE);

    /* ZOMBIE -> FREE (via proc_free) */
    proc_free(p);
    assert(p->state == PROC_STATE_FREE);

    return 0;
}
DEFINE_SELFTEST(proc_state_machine, selftest_proc_state_machine);

static i32 selftest_proc_sleeping_transition(void)
{
    struct proc *p = proc_alloc();
    assert(p != NULL);
    proc_set_state(p, PROC_STATE_RUNNING);

    proc_set_state(p, PROC_STATE_SLEEPING);
    assert(p->state == PROC_STATE_SLEEPING);

    proc_set_state(p, PROC_STATE_RUNNING);
    assert(p->state == PROC_STATE_RUNNING);

    /* Cleanup. */
    proc_set_state(p, PROC_STATE_ZOMBIE);
    proc_free(p);
    return 0;
}
DEFINE_SELFTEST(proc_sleeping_transition, selftest_proc_sleeping_transition);

static i32 selftest_proc_reparent_orphan(void)
{
    /* Allocate "init" (PID 1) as the reparent target. */
    u16 save_next = next_pid;
    next_pid = 1;
    struct proc *init = proc_alloc();
    assert(init != NULL);
    assert(init->pid == 1);
    proc_set_state(init, PROC_STATE_RUNNING);

    /* Allocate parent and two children. */
    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    struct proc *c1 = proc_alloc();
    assert(c1 != NULL);
    c1->parent_pid = parent->pid;
    proc_set_state(c1, PROC_STATE_RUNNING);

    struct proc *c2 = proc_alloc();
    assert(c2 != NULL);
    c2->parent_pid = parent->pid;
    proc_set_state(c2, PROC_STATE_RUNNING);

    assert(proc_count_children(parent->pid) == 2);

    /* Parent exits; children should be reparented to PID 1. */
    proc_reparent_children(parent);

    assert(c1->parent_pid == 1);
    assert(c2->parent_pid == 1);
    assert(proc_count_children(init->pid) == 2);
    assert(proc_count_children(parent->pid) == 0);

    /* Cleanup. */
    proc_set_state(c1, PROC_STATE_ZOMBIE);
    proc_free(c1);
    proc_set_state(c2, PROC_STATE_ZOMBIE);
    proc_free(c2);
    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    proc_set_state(init, PROC_STATE_ZOMBIE);
    proc_free(init);
    next_pid = save_next;
    return 0;
}
DEFINE_SELFTEST(proc_reparent_orphan, selftest_proc_reparent_orphan);

static i32 selftest_proc_reparent_zombie(void)
{
    u16 save_next = next_pid;
    next_pid = 1;
    struct proc *init = proc_alloc();
    assert(init != NULL && init->pid == 1);
    proc_set_state(init, PROC_STATE_RUNNING);

    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    struct proc *child = proc_alloc();
    assert(child != NULL);
    child->parent_pid = parent->pid;
    proc_set_state(child, PROC_STATE_RUNNING);
    proc_set_state(child, PROC_STATE_ZOMBIE); /* child died first */
    u16 child_pid = child->pid;

    /* Parent exits; zombie child should be reparented to init. */
    proc_reparent_children(parent);
    assert(child->parent_pid == 1);
    assert(proc_count_children(init->pid) == 1);

    /* Verify child is still findable as zombie under init. */
    struct proc *found = proc_find(child_pid);
    assert(found == child);
    assert(found->state == PROC_STATE_ZOMBIE);

    /* Cleanup: free zombie child, then parent and init. */
    proc_free(child);
    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    proc_set_state(init, PROC_STATE_ZOMBIE);
    proc_free(init);
    next_pid = save_next;
    return 0;
}
DEFINE_SELFTEST(proc_reparent_zombie, selftest_proc_reparent_zombie);

/* Test multi-child wait: spawn 3 children, make zombies, wait all. */
static i32 selftest_proc_multi_child_wait(void)
{
    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    struct proc *children[3];
    for (sz i = 0; i < 3; i++) {
        children[i] = proc_alloc();
        assert(children[i] != NULL);
        children[i]->parent_pid = parent->pid;
        proc_set_state(children[i], PROC_STATE_RUNNING);
    }

    /* All children exit (become zombies). */
    for (sz i = 0; i < 3; i++) {
        children[i]->exit_code = (i32) (10 + i);
        proc_set_state(children[i], PROC_STATE_ZOMBIE);
    }

    /* Wait for all 3 children via proc_wait_child. */
    for (sz i = 0; i < 3; i++) {
        u16 pid;
        i32 code;
        i32 rc = proc_wait_child(parent, &pid, &code);
        assert(rc == 0);
        assert(pid > 0);
    }

    /* No more children -> ECHILD. */
    u16 pid;
    i32 code;
    assert(proc_wait_child(parent, &pid, &code) == -(i32) ECHILD);

    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    return 0;
}
DEFINE_SELFTEST(proc_multi_child_wait, selftest_proc_multi_child_wait);

/* Test proc_exit: full exit sequence with reparenting. */
static i32 selftest_proc_exit_lifecycle(void)
{
    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    struct proc *child = proc_alloc();
    assert(child != NULL);
    child->parent_pid = parent->pid;
    proc_set_state(child, PROC_STATE_RUNNING);

    u16 child_pid = child->pid;

    /* Child exits via proc_exit. */
    proc_exit(child, 42);

    /* Child should be zombie (parent is alive). */
    struct proc *found = proc_find(child_pid);
    assert(found == child);
    assert(found->state == PROC_STATE_ZOMBIE);
    assert(found->exit_code == 42);

    /* Parent reaps. */
    u16 pid;
    i32 code;
    i32 rc = proc_wait_child(parent, &pid, &code);
    assert(rc == 0);
    assert(pid == child_pid);
    assert(code == 42);

    /* Child slot now FREE and unreachable. */
    assert(proc_find(child_pid) == NULL);

    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    return 0;
}
DEFINE_SELFTEST(proc_exit_lifecycle, selftest_proc_exit_lifecycle);

/* Test resource cleanup: proc slot is reusable after proc_exit + wait. */
static i32 selftest_proc_slot_reuse(void)
{
    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    struct proc *child = proc_alloc();
    assert(child != NULL);
    child->parent_pid = parent->pid;
    proc_set_state(child, PROC_STATE_RUNNING);

    u32 gen = child->generation;

    proc_exit(child, 0);

    u16 pid;
    i32 code;
    proc_wait_child(parent, &pid, &code);

    /* After reap, the slot should be FREE with incremented generation. */
    assert(child->state == PROC_STATE_FREE);
    assert(child->generation == gen + 1);

    /* Can allocate again in that slot. */
    struct proc *reused = proc_alloc();
    assert(reused != NULL);
    /* The allocator may or may not give us the same slot, but the table
     * should have capacity.
     */
    proc_free(reused);

    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    return 0;
}
DEFINE_SELFTEST(proc_slot_reuse, selftest_proc_slot_reuse);

/* Test proc_count_children counts correctly. */
static i32 selftest_proc_count_children(void)
{
    struct proc *parent = proc_alloc();
    assert(parent != NULL);
    proc_set_state(parent, PROC_STATE_RUNNING);

    assert(proc_count_children(parent->pid) == 0);

    struct proc *c1 = proc_alloc();
    assert(c1 != NULL);
    c1->parent_pid = parent->pid;
    proc_set_state(c1, PROC_STATE_RUNNING);
    assert(proc_count_children(parent->pid) == 1);

    struct proc *c2 = proc_alloc();
    assert(c2 != NULL);
    c2->parent_pid = parent->pid;
    proc_set_state(c2, PROC_STATE_RUNNING);
    assert(proc_count_children(parent->pid) == 2);

    /* Zombie still counts. */
    proc_set_state(c1, PROC_STATE_ZOMBIE);
    assert(proc_count_children(parent->pid) == 2);

    /* Free removes from count. */
    proc_free(c1);
    assert(proc_count_children(parent->pid) == 1);

    proc_set_state(c2, PROC_STATE_ZOMBIE);
    proc_free(c2);
    assert(proc_count_children(parent->pid) == 0);

    proc_set_state(parent, PROC_STATE_ZOMBIE);
    proc_free(parent);
    return 0;
}
DEFINE_SELFTEST(proc_count_children, selftest_proc_count_children);
