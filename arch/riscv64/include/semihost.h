/* SPDX-License-Identifier: MIT */

#ifndef MAZU_SEMIHOST_H
#define MAZU_SEMIHOST_H

#if CONFIG_SEMIHOSTING

#include <mazu/base.h>
#include <mazu/stringdef.h>

/* ARM semihosting operation numbers (adopted by RISC-V). */
#define SYS_OPEN 0x01
#define SYS_CLOSE 0x02
#define SYS_WRITEC 0x03
#define SYS_WRITE0 0x04
#define SYS_WRITE 0x05
#define SYS_READ 0x06
#define SYS_READC 0x07
#define SYS_SEEK 0x0A
#define SYS_FLEN 0x0C
#define SYS_CLOCK 0x10
#define SYS_TIME 0x11
#define SYS_SYSTEM 0x12
#define SYS_ERRNO 0x13
#define SYS_GET_CMDLINE 0x15
#define SYS_EXIT 0x18

/* ADP reason code for normal application exit. */
#define ADP_STOPPED_APPLICATION_EXIT 0x20026

/* Trap primitives (arch/riscv64/semihost.c). */
u64 semihost_call(u64 op, void *params);
__noreturn void semihost_call_noreturn(u64 op, void *params);

/* M2: Console I/O. */
void semihost_putc(char c);
void semihost_puts(const char *s);
void semihost_write_str(struct str s);
i32 semihost_getc(void);

/* M3: Host file I/O. */
i32 semihost_open(const char *path, i32 mode, sz path_len);
i32 semihost_close(i32 fd);
sz semihost_read(i32 fd, void *buf, sz len);
sz semihost_write(i32 fd, const void *buf, sz len);
i32 semihost_seek(i32 fd, sz pos);
i64 semihost_flen(i32 fd);

/* M4: System operations. */
__noreturn void semihost_exit(i32 code);
i32 semihost_get_cmdline(char *buf, sz len, sz *actual_len);
i64 semihost_clock(void);
u64 semihost_time(void);
i32 semihost_errno(void);
i32 semihost_system(const char *cmd, sz cmd_len);

#endif /* CONFIG_SEMIHOSTING */
#endif /* MAZU_SEMIHOST_H */
