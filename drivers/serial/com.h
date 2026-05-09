/* SPDX-License-Identifier: MIT */
#ifndef MAZU_COM
#define MAZU_COM

#include <mazu/base.h>
#include <mazu/errordef.h>
#include <mazu/stringdef.h>

struct result com_init(u16 port);
struct result com_write(u16 port, struct str str);

#endif /* MAZU_COM */
