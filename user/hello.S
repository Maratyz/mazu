/* Minimal user-space hello program.
 *
 * Writes "hello\n" to stdout (fd 1) via SYS_WRITE, then exits via SYS_EXIT.
 * Built as a flat binary loaded at USER_CODE_BASE (0x10000).
 */

#include <mazu/syscall.h>

    .section .text
    .globl _start
_start:
    /* SYS_WRITE(fd=1, buf=msg, len=6) */
    li   a7, SYS_WRITE
    li   a0, 1
    lla  a1, msg
    li   a2, 6
    ecall

    /* SYS_EXIT(code=0) */
    li   a7, SYS_EXIT
    li   a0, 0
    ecall

    /* Should not reach here, but spin just in case. */
1:  j    1b

    .section .rodata
msg:
    .ascii "hello\n"
