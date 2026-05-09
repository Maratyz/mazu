/* SPDX-License-Identifier: MIT */
/* RISC-V 64-bit semihosting implementation.
 *
 * The RISC-V semihosting specification mandates a 3-instruction trap sequence
 * (slli/ebreak/srai) aligned to 16 bytes. QEMU recognizes this pattern as a
 * semihosting call rather than a plain breakpoint when started with
 * -semihosting-config enable=on,target=native.
 */

#include <mazu/base.h>
#include <semihost.h>

/* Trap primitives
 *
 * Use extended asm with explicit register constraints so the compiler cannot
 * eliminate the parameter stores via dead-store elimination. The .align 4
 * (= 16-byte) inside the asm satisfies the semihosting alignment requirement.
 */

u64 semihost_call(u64 op, void *params)
{
    register u64 a0 __asm__("a0") = op;
    register u64 a1 __asm__("a1") = (u64) params;
    __asm__ volatile(
        ".align 4\n"
        ".option push\n"
        ".option norvc\n"
        "slli zero, zero, 0x1f\n"
        "ebreak\n"
        "srai zero, zero, 0x7\n"
        ".option pop\n"
        : "+r"(a0)
        : "r"(a1)
        : "memory");
    return a0;
}

__noreturn void semihost_call_noreturn(u64 op, void *params)
{
    register u64 a0 __asm__("a0") = op;
    register u64 a1 __asm__("a1") = (u64) params;
    while (true) {
        __asm__ volatile(
            ".align 4\n"
            ".option push\n"
            ".option norvc\n"
            "slli zero, zero, 0x1f\n"
            "ebreak\n"
            "srai zero, zero, 0x7\n"
            ".option pop\n"
            : "+r"(a0)
            : "r"(a1)
            : "memory");
    }
}

/* M2: Console I/O */

void semihost_putc(char c)
{
    semihost_call(SYS_WRITEC, &c);
}

void semihost_puts(const char *s)
{
    semihost_call(SYS_WRITE0, (void *) s);
}

/* Lazily-opened stdout handle via semihosting ":tt" (mode 4 = write). */
static i32 semi_stdout_fd = -1;

static i32 semi_stdout(void)
{
    if (semi_stdout_fd < 0)
        semi_stdout_fd = semihost_open(":tt", 4, 3);
    return semi_stdout_fd;
}

/* Write a fat string to the host console using SYS_WRITE (length-based).
 * Binary-safe - embedded NUL bytes are transmitted faithfully.
 */
void semihost_write_str(struct str s)
{
    i32 fd = semi_stdout();
    if (fd < 0 || s.len <= 0)
        return;
    semihost_write(fd, s.dat, s.len);
}

i32 semihost_getc(void)
{
    return (i32) semihost_call(SYS_READC, NULL);
}

/* M3: Host file I/O */

i32 semihost_open(const char *path, i32 mode, sz path_len)
{
    u64 params[3] = {(u64) path, (u64) mode, (u64) path_len};
    return (i32) semihost_call(SYS_OPEN, params);
}

i32 semihost_close(i32 fd)
{
    u64 params[1] = {(u64) fd};
    return (i32) semihost_call(SYS_CLOSE, params);
}

/* SYS_READ returns bytes NOT transferred.  Invert to bytes transferred. */
sz semihost_read(i32 fd, void *buf, sz len)
{
    u64 params[3] = {(u64) fd, (u64) buf, (u64) len};
    sz not_read = (sz) semihost_call(SYS_READ, params);
    return len - not_read;
}

/* SYS_WRITE returns bytes NOT transferred.  Invert to bytes transferred. */
sz semihost_write(i32 fd, const void *buf, sz len)
{
    u64 params[3] = {(u64) fd, (u64) buf, (u64) len};
    sz not_written = (sz) semihost_call(SYS_WRITE, params);
    return len - not_written;
}

i32 semihost_seek(i32 fd, sz pos)
{
    u64 params[2] = {(u64) fd, (u64) pos};
    return (i32) semihost_call(SYS_SEEK, params);
}

i64 semihost_flen(i32 fd)
{
    u64 params[1] = {(u64) fd};
    return (i64) semihost_call(SYS_FLEN, params);
}

__noreturn void semihost_exit(i32 code)
{
    u64 params[2] = {ADP_STOPPED_APPLICATION_EXIT, (u64) code};
    semihost_call_noreturn(SYS_EXIT, params);
}

i32 semihost_get_cmdline(char *buf, sz len, sz *actual_len)
{
    u64 params[2] = {(u64) buf, (u64) len};
    i32 ret = (i32) semihost_call(SYS_GET_CMDLINE, params);
    if (ret == 0 && actual_len)
        *actual_len = (sz) params[1];
    return ret;
}

i64 semihost_clock(void)
{
    return (i64) semihost_call(SYS_CLOCK, NULL);
}

u64 semihost_time(void)
{
    return semihost_call(SYS_TIME, NULL);
}

i32 semihost_errno(void)
{
    return (i32) semihost_call(SYS_ERRNO, NULL);
}

i32 semihost_system(const char *cmd, sz cmd_len)
{
    u64 params[2] = {(u64) cmd, (u64) cmd_len};
    return (i32) semihost_call(SYS_SYSTEM, params);
}
