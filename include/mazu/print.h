/* SPDX-License-Identifier: MIT */
#ifndef MAZU_PRINT_H
#define MAZU_PRINT_H

#include <mazu/base.h>
#include <mazu/errordef.h>
#include <mazu/stringdef.h>

struct result print_str(struct str str);
struct result print_fmt(struct str_buf buf, struct str fmt, ...);

/* Console write function type (for klog tee and driver registration). */
typedef struct result (*console_write_fn_t)(struct str);

/* Register a console backend (e.g. UART driver calls this after init). */
void print_register_console(console_write_fn_t writer);

/* Redirect console output to a klog tee writer.  Stores the current
 * console_write function pointer into *old_writer so the tee can
 * chain to the original output path.
 */
void print_switch_to_klog(console_write_fn_t klog_writer,
                          console_write_fn_t *old_writer);

/* Log levels (lower value = higher priority, always printed).
 * Inspired by Linux kernel printk levels (see printk-basics.html).
 * Messages with level > __DEBUG__ are compiled out.
 * KERN_ERR and KERN_WARNING share level 0 so both print at DEBUG=0
 * (the default "warnings/errors" verbosity).
 */
#define KERN_ERR 0
#define KERN_WARNING 0
#define KERN_INFO 1
#define KERN_DEBUG 2
#define KERN_VERBOSE 3

struct result __printk(struct str basename,
                       struct str line,
                       struct str funcname,
                       i16 level,
                       struct str fmt,
                       ...);

#define printk(level, fmt, ...)                                             \
    __printk(                                                               \
        STR(__BASENAME__), STR(TOSTRING(__LINE__)),                         \
        (struct str) {.dat = (char *) __func__, .len = lengthof(__func__)}, \
        level, fmt, ##__VA_ARGS__)

/* printk() and the pr_* wrappers rely on STR() so log call sites can stay in
 * the struct str world even for file names, line numbers, and format strings.
 */
#define pr_err(fmt, ...) printk(KERN_ERR, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KERN_DEBUG, fmt, ##__VA_ARGS__)

#endif /* MAZU_PRINT_H */
