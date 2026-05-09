/* SPDX-License-Identifier: MIT */
/* Runtime configuration */

#ifndef MAZU_RTCFG_H
#define MAZU_RTCFG_H

#include <mazu/arena.h>
#include <mazu/error.h>
#include <mazu/net/ip_addr.h>
#include <mazu/ramfs.h>
#include <mazu/string.h>

struct runtime_config {
    struct option_ipv4_addr host_ip, local_ip, local_ip_mask,
        default_gateway_ip;
};

struct_result(runtime_config, struct runtime_config *);

struct result_runtime_config rtcfg_read_config(struct ram_fs *rfs,
                                               struct str cfg_filename,
                                               struct arena arn);

#endif /* MAZU_RTCFG_H */