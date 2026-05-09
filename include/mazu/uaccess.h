/* SPDX-License-Identifier: MIT */
/* User-space memory access helpers.
 *
 * copy_to_user / copy_from_user toggle sstatus.SUM around a memcpy
 * and validate that the user pointer falls within the user-space
 * address range.  Returns 0 on success, -EFAULT on bounds violation.
 */

#ifndef MAZU_UACCESS_H
#define MAZU_UACCESS_H

#include <mazu/base.h>

struct trap_frame;

/* User-space address range.  Any address outside this window is rejected. */
#define USER_CODE_BASE 0x00010000UL
#define USER_DATA_BASE 0x00020000UL
#define USER_STACK_TOP 0x40000000UL
#define USER_STACK_SIZE (4UL * PAGE_SIZE)

/* Check whether [uaddr, uaddr+len) is a valid user-accessible range.
 * Returns true if the range falls within the user address window and
 * every covered page has PTE_U set.  Does not fault.
 */
bool user_addr_valid(ptr uaddr, sz len);

/* Check whether [uaddr, uaddr+len) is a valid user-writable range.
 * Like user_addr_valid but also verifies PTE_W on every covered page.
 * Use for destinations of copy_to_user / pipe_read pre-validation.
 */
bool user_addr_writable(ptr uaddr, sz len);

/* Copy 'len' bytes from kernel buffer 'src' to user address 'udst'.
 * Returns 0 on success, -EFAULT if udst is not a valid user address.
 */
__must_check i64 copy_to_user(ptr udst, const void *src, sz len);

/* Copy 'len' bytes from user address 'usrc' to kernel buffer 'dst'.
 * Returns 0 on success, -EFAULT if usrc is not a valid user address.
 */
__must_check i64 copy_from_user(void *dst, ptr usrc, sz len);

/* Copy 'len' bytes from kernel buffer 'src' to user address 'udst',
 * validating the destination against process 'p' instead of the calling
 * task.  Used by the loader to write into a child process's VA space
 * during spawn.  Returns 0 on success, -EFAULT on validation failure.
 */
struct proc;
__must_check i64 copy_to_user_proc(struct proc *p,
                                   ptr udst,
                                   const void *src,
                                   sz len);

/* Trap-time uaccess fault handling.
 * Returns true if the fault was recognized as a recoverable uaccess fault and
 * tf was rewritten to continue at the uaccess recovery label.
 */
bool uaccess_handle_page_fault(struct trap_frame *tf);

#endif /* MAZU_UACCESS_H */
