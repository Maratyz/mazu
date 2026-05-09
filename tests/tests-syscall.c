/* SPDX-License-Identifier: MIT */
#include <mazu/selftest.h>
#include <mazu/syscall.h>
#include <mazu/uaccess.h>

static struct proc *alloc_running_proc(void)
{
    struct proc *p = proc_alloc();
    if (p)
        proc_set_state(p, PROC_STATE_RUNNING);
    return p;
}

static struct sched_task *alloc_mock_task(void)
{
    struct option_byte_array td_mem =
        kvalloc_alloc(sizeof(struct sched_task), alignof(struct sched_task));
    if (td_mem.is_none)
        return NULL;
    struct sched_task *td = byte_array_ptr(option_byte_array_checked(td_mem));
    memset(td, 0, sizeof(*td));
    return td;
}

static void free_mock_task(struct sched_task *td)
{
    kvalloc_free(byte_array_new((byte *) td, sizeof(*td)));
}

/* Allocate a RUNNING proc + mock task, linked together. */
static bool alloc_proc_and_task(struct proc **out_p, struct sched_task **out_td)
{
    struct proc *p = alloc_running_proc();
    if (!p)
        return false;
    struct sched_task *td = alloc_mock_task();
    if (!td) {
        proc_set_state(p, PROC_STATE_ZOMBIE);
        proc_free(p);
        return false;
    }
    td->proc = p;
    p->task = td;
    *out_p = p;
    *out_td = td;
    return true;
}

/* Teardown: transition to ZOMBIE, free proc, free task. */
static void free_proc_and_task(struct proc *p, struct sched_task *td)
{
    proc_set_state(p, PROC_STATE_ZOMBIE);
    proc_free(p);
    free_mock_task(td);
}

static i32 selftest_sys_open_emfile(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* Fill all FD slots so sys_open returns EMFILE. */
    for (sz i = PROC_FD_STDERR + 1; i < PROC_FD_MAX; i++)
        p->fd_table[i].is_open = true;

    struct trap_frame tf = {0};
    tf.a0 = USER_CODE_BASE;
    tf.a1 = 4;
    assert(sys_open(&tf, td) == -(i64) EMFILE);

    for (sz i = PROC_FD_STDERR + 1; i < PROC_FD_MAX; i++)
        p->fd_table[i].is_open = false;
    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_open_emfile, selftest_sys_open_emfile);

static i32 selftest_sys_exit_frees_proc_slot(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));
    u16 pid = p->pid;

    struct trap_frame tf = {0};
    tf.a0 = 7;

    assert(sys_exit(&tf, td) == 0);
    assert(td->state == TD_STATE_TERMINATING);
    assert(td->proc == NULL);
    /* Orphan auto-reap: parent_pid=0, no parent exists -> freed. */
    assert(proc_find(pid) == NULL);
    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(sys_exit_frees_proc_slot, selftest_sys_exit_frees_proc_slot);

static i32 selftest_syscall_enosys(void)
{
    struct trap_frame tf = {0};
    tf.a7 = SYS_NR + 5; /* out of range */
    assert(syscall_dispatch(&tf, NULL) == -(i64) ENOSYS);
    return 0;
}
DEFINE_SELFTEST(syscall_enosys, selftest_syscall_enosys);

static i32 selftest_syscall_needs_proc(void)
{
    struct sched_task *td = alloc_mock_task();
    assert(td != NULL);
    td->proc = NULL; /* no process */

    struct trap_frame tf = {0};
    tf.a7 = SYS_OPEN; /* requires NEEDS_PROC */
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(syscall_needs_proc, selftest_syscall_needs_proc);

static i32 selftest_syscall_needs_proc_new_handlers(void)
{
    struct sched_task *td = alloc_mock_task();
    assert(td != NULL);
    td->proc = NULL;

    struct trap_frame tf = {0};

    tf.a7 = SYS_MUTEX_INIT;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_BARRIER_INIT;
    tf.a0 = 2;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_RWLOCK_INIT;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_MQ_OPEN;
    tf.a0 = 1;
    tf.a1 = 16;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_TIMER_SETTIME;
    tf.a0 = 0;
    tf.a1 = 1;
    tf.a2 = 0;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_SCHED_SETAFFINITY;
    tf.a0 = 0;
    tf.a1 = -1;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    tf.a7 = SYS_SCHED_GETAFFINITY;
    tf.a0 = 0;
    tf.a1 = USER_CODE_BASE;
    assert(syscall_dispatch(&tf, td) == -(i64) EPERM);

    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(syscall_needs_proc_new_handlers,
                selftest_syscall_needs_proc_new_handlers);

static i32 selftest_syscall_allow_mask(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* Allow only SYS_EXIT and SYS_YIELD. */
    p->syscall_allow[0] = BIT(SYS_EXIT) | BIT(SYS_YIELD);

    struct trap_frame tf = {0};

    /* SYS_OPEN should be denied (not in allow mask). */
    tf.a7 = SYS_OPEN;
    assert(syscall_dispatch(&tf, td) == -(i64) EACCES);

    /* SYS_YIELD should be allowed. */
    tf.a7 = SYS_YIELD;
    assert(syscall_dispatch(&tf, td) == 0);

    /* SYS_EXIT terminates, so test it last. */
    tf.a7 = SYS_EXIT;
    tf.a0 = 0;
    assert(syscall_dispatch(&tf, td) == 0);

    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(syscall_allow_mask, selftest_syscall_allow_mask);

static i32 selftest_syscall_allow_mask_high_numbers(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    p->syscall_allow[1] = BIT(SYS_TIMER_GETTIME - 64);

    struct trap_frame tf = {0};
    tf.a7 = SYS_TIMER_GETTIME;
    tf.a0 = -1;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    tf.a7 = SYS_THREAD_SELF;
    assert(syscall_dispatch(&tf, td) == -(i64) EACCES);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(syscall_allow_mask_high_numbers,
                selftest_syscall_allow_mask_high_numbers);

static i32 selftest_timer_invalid_handles(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    struct trap_frame tf = {0};

    tf.a7 = SYS_TIMER_GETTIME;
    tf.a0 = (u64) -1;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    tf.a7 = SYS_TIMER_GETOVERRUN;
    tf.a0 = (u64) -1;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(timer_invalid_handles, selftest_timer_invalid_handles);

static i32 selftest_syscall_security_stats(void)
{
    struct syscall_security_stats before = syscall_security_stats_get();

    /* Trigger ENOSYS counter. */
    struct trap_frame tf = {0};
    tf.a7 = SYS_NR + 1;
    syscall_dispatch(&tf, NULL);

    struct syscall_security_stats after = syscall_security_stats_get();
    assert(after.nr_enosys > before.nr_enosys);

    /* Trigger nr_denied counter via allow-list denial. */
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));
    p->syscall_allow[0] = BIT(SYS_EXIT);

    before = syscall_security_stats_get();
    tf.a7 = SYS_YIELD; /* not in allow mask -> EACCES -> nr_denied++ */
    syscall_dispatch(&tf, td);
    after = syscall_security_stats_get();
    assert(after.nr_denied > before.nr_denied);

    /* Cleanup. */
    p->syscall_allow[0] = 0;
    p->syscall_allow[1] = 0;
    tf.a7 = SYS_EXIT;
    tf.a0 = 0;
    syscall_dispatch(&tf, td);
    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(syscall_security_stats, selftest_syscall_security_stats);

static i32 selftest_syscall_unrestricted(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));
    assert(p->syscall_allow[0] == 0 && p->syscall_allow[1] == 0);

    struct trap_frame tf = {0};

    /* All syscalls should be allowed when mask is 0. */
    tf.a7 = SYS_YIELD;
    assert(syscall_dispatch(&tf, td) == 0);
    tf.a7 = SYS_TIME;
    assert(syscall_dispatch(&tf, td) >= 0);

    tf.a7 = SYS_EXIT;
    tf.a0 = 0;
    syscall_dispatch(&tf, td);
    free_mock_task(td);
    return 0;
}
DEFINE_SELFTEST(syscall_unrestricted, selftest_syscall_unrestricted);

static i32 selftest_sys_getpid(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    struct trap_frame tf = {0};
    tf.a7 = SYS_GETPID;
    i64 pid = syscall_dispatch(&tf, td);
    assert(pid == (i64) p->pid);
    assert(pid > 0);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_getpid, selftest_sys_getpid);

static i32 selftest_sys_getppid(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));
    p->parent_pid = 42;

    struct trap_frame tf = {0};
    tf.a7 = SYS_GETPPID;
    i64 ppid = syscall_dispatch(&tf, td);
    assert(ppid == 42);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_getppid, selftest_sys_getppid);

static i32 selftest_sys_dup(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* stdout (FD 1) is open; dup it. Lowest free is FD 3. */
    struct trap_frame tf = {0};
    tf.a7 = SYS_DUP;
    tf.a0 = PROC_FD_STDOUT;
    i64 newfd = syscall_dispatch(&tf, td);
    assert(newfd == 3);
    assert(p->fd_table[3].is_open);

    /* Dup again; should get FD 4. */
    tf.a0 = PROC_FD_STDOUT;
    newfd = syscall_dispatch(&tf, td);
    assert(newfd == 4);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_dup, selftest_sys_dup);

static i32 selftest_sys_dup_ebadf(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    struct trap_frame tf = {0};
    tf.a7 = SYS_DUP;
    tf.a0 = 10; /* FD 10 is not open */
    assert(syscall_dispatch(&tf, td) == -(i64) EBADF);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_dup_ebadf, selftest_sys_dup_ebadf);

static i32 selftest_sys_dup2(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* dup2(stdout, 5) - FD 5 was closed, now open. */
    struct trap_frame tf = {0};
    tf.a7 = SYS_DUP2;
    tf.a0 = PROC_FD_STDOUT;
    tf.a1 = 5;
    i64 rc = syscall_dispatch(&tf, td);
    assert(rc == 5);
    assert(p->fd_table[5].is_open);

    /* dup2(stdout, stdout) is a no-op, returns stdout. */
    tf.a0 = PROC_FD_STDOUT;
    tf.a1 = PROC_FD_STDOUT;
    rc = syscall_dispatch(&tf, td);
    assert(rc == PROC_FD_STDOUT);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_dup2, selftest_sys_dup2);

static i32 selftest_sys_lseek(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* Set up a fake seekable FD without opening a real file.
     * sys_lseek only touches fd_table[fd].offset / is_seekable.
     */
    i32 fd = 3;
    p->fd_table[fd].is_open = true;
    p->fd_table[fd].is_seekable = true;
    p->fd_table[fd].offset = 0;

    /* SEEK_SET to position 10. */
    struct trap_frame tf = {0};
    tf.a0 = (u64) fd;
    tf.a1 = 10;
    tf.a2 = SEEK_SET;
    i64 pos = sys_lseek(&tf, td);
    assert(pos == 10);
    assert(p->fd_table[fd].offset == 10);

    /* SEEK_CUR +5 -> position 15. */
    tf.a1 = 5;
    tf.a2 = SEEK_CUR;
    pos = sys_lseek(&tf, td);
    assert(pos == 15);

    /* SEEK_SET negative -> EINVAL. */
    tf.a1 = (u64) (i64) -1;
    tf.a2 = SEEK_SET;
    assert(sys_lseek(&tf, td) == -(i64) EINVAL);

    /* Console FDs are not seekable -> ESPIPE. */
    tf.a0 = PROC_FD_STDOUT;
    tf.a1 = 0;
    tf.a2 = SEEK_SET;
    assert(sys_lseek(&tf, td) == -(i64) ESPIPE);

    /* Cleanup. */
    p->fd_table[fd].is_open = false;
    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_lseek, selftest_sys_lseek);

/* sys_chdir and sys_getcwd use copy_from_user/copy_to_user, which reject
 * kernel pointers in S-mode selftests.  Test the core logic by:
 * (1) verifying initial cwd state, (2) directly manipulating cwd to
 * simulate chdir, (3) verifying getcwd ERANGE boundary.
 */

static i32 selftest_sys_chdir_getcwd(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* Initial cwd is "/". */
    assert(p->cwd_len == 1);
    assert(p->cwd[0] == '/');

    /* Simulate chdir: directly set cwd as sys_chdir would after
     * VFS validation and copy_user_path succeed.
     */
    const char *newcwd = "/web";
    sz newlen = 4;
    memcpy(p->cwd, newcwd, newlen);
    p->cwd_len = newlen;
    assert(p->cwd[0] == '/' && p->cwd[1] == 'w');

    /* getcwd boundary: size < cwd_len+1 should produce ERANGE
     * (POSIX requires space for NUL terminator). cwd_len=4 ("/web"),
     * so need size >= 5.
     */
    struct trap_frame tf = {0};
    tf.a0 = 0; /* NULL buffer - never reached due to size check */
    tf.a1 = 0; /* size = 0 */
    assert(sys_getcwd(&tf, td) == -(i64) ERANGE);

    tf.a1 = 4; /* exactly cwd_len, still needs +1 for NUL */
    assert(sys_getcwd(&tf, td) == -(i64) ERANGE);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_chdir_getcwd, selftest_sys_chdir_getcwd);

/* sysconf tests: call sys_sysconf_query() directly. */

static i32 selftest_sysconf_page_size(void)
{
    assert(sys_sysconf_query(_SC_PAGE_SIZE) == (i64) PAGE_SIZE);
    assert(sys_sysconf_query(_SC_PAGESIZE) == (i64) PAGE_SIZE);
    return 0;
}
DEFINE_SELFTEST(sysconf_page_size, selftest_sysconf_page_size);

static i32 selftest_sysconf_open_max(void)
{
    assert(sys_sysconf_query(_SC_OPEN_MAX) == (i64) PROC_FD_MAX);
    return 0;
}
DEFINE_SELFTEST(sysconf_open_max, selftest_sysconf_open_max);

static i32 selftest_sysconf_nproc(void)
{
    assert(sys_sysconf_query(_SC_NPROCESSORS_CONF) == (i64) nr_cpus_online);
    assert(sys_sysconf_query(_SC_NPROCESSORS_ONLN) == (i64) nr_cpus_online);
    return 0;
}
DEFINE_SELFTEST(sysconf_nproc, selftest_sysconf_nproc);

static i32 selftest_sysconf_pipe_buf(void)
{
    assert(sys_sysconf_query(_SC_PIPE_BUF) == (i64) PIPE_BUF_SIZE);
    return 0;
}
DEFINE_SELFTEST(sysconf_pipe_buf, selftest_sysconf_pipe_buf);

static i32 selftest_sysconf_child_max(void)
{
    assert(sys_sysconf_query(_SC_CHILD_MAX) == (i64) (PROC_MAX - 1));
    return 0;
}
DEFINE_SELFTEST(sysconf_child_max, selftest_sysconf_child_max);

static i32 selftest_sysconf_invalid(void)
{
    assert(sys_sysconf_query(9999) == -(i64) EINVAL);
    assert(sys_sysconf_query(-1) == -(i64) EINVAL);
    return 0;
}
DEFINE_SELFTEST(sysconf_invalid, selftest_sysconf_invalid);

static i32 selftest_sys_sched_setaffinity(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    /* pid=0 means self: pin to hart 0. */
    struct trap_frame tf = {0};
    tf.a7 = SYS_SCHED_SETAFFINITY;
    tf.a0 = 0; /* self */
    tf.a1 = 0; /* hart 0 */
    assert(syscall_dispatch(&tf, td) == 0);
    assert(__atomic_load_n(&td->td_affinity, __ATOMIC_ACQUIRE) == 0);

    /* Reset to any. */
    tf.a1 = (u64) (i64) -1;
    assert(syscall_dispatch(&tf, td) == 0);
    assert(__atomic_load_n(&td->td_affinity, __ATOMIC_ACQUIRE) == -1);

    /* Invalid hart -> EINVAL. */
    tf.a1 = (u64) 9999;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    /* Overflow: pid exceeds u16 range -> EINVAL. */
    tf.a0 = (u64) U16_MAX + 1;
    tf.a1 = 0;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_sched_setaffinity, selftest_sys_sched_setaffinity);

/* sys_sched_getaffinity uses copy_to_user, which rejects kernel pointers
 * in S-mode selftests.  Verify the core logic by checking td_affinity
 * directly after setaffinity, and test getaffinity error paths.
 */
static i32 selftest_sys_sched_getaffinity_errors(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    struct trap_frame tf = {0};
    tf.a7 = SYS_SCHED_GETAFFINITY;

    /* Overflow: pid exceeds u16 range -> EINVAL. */
    tf.a0 = (u64) U16_MAX + 1;
    tf.a1 = 0;
    assert(syscall_dispatch(&tf, td) == -(i64) EINVAL);

    /* Bad pid -> ESRCH. */
    tf.a0 = (u64) U16_MAX;  /* unlikely to exist */
    tf.a1 = USER_CODE_BASE; /* valid user address */
    assert(syscall_dispatch(&tf, td) == -(i64) ESRCH);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_sched_getaffinity_errors,
                selftest_sys_sched_getaffinity_errors);

static i32 selftest_sys_sched_getaffinity_success(void)
{
    struct proc *p;
    struct sched_task *td;
    assert(alloc_proc_and_task(&p, &td));

    const vaddr_t user_out = USER_DATA_BASE + (129UL * PAGE_SIZE);
    assert(
        proc_map_user_page(p, user_out, PT_FLAG_RW | PT_FLAG_USER).is_error ==
        false);

    __atomic_store_n(&td->td_affinity, 2, __ATOMIC_RELEASE);

    struct trap_frame tf = {0};
    tf.a7 = SYS_SCHED_GETAFFINITY;
    tf.a0 = 0;
    tf.a1 = user_out;
    assert(syscall_dispatch(&tf, td) == 0);

    i32 affinity = -1;
    assert(copy_from_user(&affinity, user_out, sizeof(affinity)) == 0);
    assert(affinity == 2);

    free_proc_and_task(p, td);
    return 0;
}
DEFINE_SELFTEST(sys_sched_getaffinity_success,
                selftest_sys_sched_getaffinity_success);
