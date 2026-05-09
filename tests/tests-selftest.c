/* SPDX-License-Identifier: MIT */
#include <mazu/errordef.h>
#include <mazu/selftest.h>
#include <mazu/string.h>

/* Verify every defined errno has a non-"Unknown" string.
 * "Unknown error" has length 13; no valid error string matches that.
 */
static i32 test_errno_strings(void)
{
    static const u16 codes[] = {
        EPERM,     ENOENT,       ESRCH,        EINTR,        EIO,
        ENXIO,     ENOEXEC,      EBADF,        ECHILD,       EAGAIN,
        ENOMEM,    EACCES,       EFAULT,       EBUSY,        EEXIST,
        ENODEV,    ENOTDIR,      EISDIR,       EINVAL,       ENFILE,
        EMFILE,    ENOTTY,       EFBIG,        ENOSPC,       ESPIPE,
        EROFS,     EPIPE,        ERANGE,       ENAMETOOLONG, ENOSYS,
        ENOTEMPTY, EMSGSIZE,     EADDRINUSE,   ECONNRESET,   ENOBUFS,
        ETIMEDOUT, ECONNREFUSED, EHOSTUNREACH,
    };
    for (sz i = 0; i < countof(codes); i++) {
        struct str s = error_code_str(codes[i]);
        /* "Unknown error" is exactly 13 chars; all valid strings contain
         * a parenthesized code suffix and are longer.
         */
        if (s.len == 13)
            return 1;
    }
    return 0;
}
DEFINE_SELFTEST(errno_strings, test_errno_strings);
