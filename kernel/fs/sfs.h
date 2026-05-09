/* SPDX-License-Identifier: MIT */
/* Simple Filesystem (SFS).
 *
 * Minimal on-disk filesystem: contiguous allocation, flat directory
 * with parent indices, bitmap free-space tracker.
 *
 * Layout:
 *   Sector 0:        Superblock
 *   Sector 1..B:     Bitmap (1 bit per sector)
 *   Sector B+1..D:   Directory table
 *   Sector D+1..N:   Data region
 */

#ifndef MAZU_SFS_H
#define MAZU_SFS_H

#include <mazu/base.h>
#include <mazu/error.h>
#include <mazu/spinlock.h>
#include <mazu/vfs.h>
#include "bcache.h"

#define SFS_MAGIC 0x5346534DUL /* "SFSM" */
#define SFS_VERSION 1
#define SFS_NAME_MAX 47

#define SFS_TYPE_FREE 0
#define SFS_TYPE_FILE 1
#define SFS_TYPE_DIR 2

#define SFS_ROOT_PARENT 0xFFFF

struct sfs_super {
    u32 magic, version, total_sectors, bitmap_start, bitmap_sectors, dir_start,
        dir_sectors, data_start;
    u32 n_dirents; /* max directory entries */
    u32 free_sectors;
    byte reserved[472];
} __packed;

static_assert(sizeof(struct sfs_super) == 512, "sfs_super must be one sector");

struct sfs_dirent {
    char name[48];    /* null-terminated */
    u32 size;         /* bytes (0 for dirs) */
    u32 start_sector; /* contiguous allocation start */
    u8 type;          /* SFS_TYPE_* */
    u8 flags;
    u16 parent_idx; /* parent directory's dirent index */
    byte reserved[4];
} __packed;

static_assert(sizeof(struct sfs_dirent) == 64, "sfs_dirent must be 64 bytes");

struct sfs {
    struct bcache *cache;
    struct sfs_super super;
    spinlock_t lock;
    char readdir_name[48]; /* persistent buffer for readdir results (matches
                            * sizeof(sfs_dirent.name), not SFS_NAME_MAX) */
};

struct result sfs_format(struct bcache *cache, u32 total_sectors);

struct_result(sfs, struct sfs *);
struct result_sfs sfs_mount(struct bcache *cache);

struct result sfs_fsck(struct sfs *fs);
struct vfs_ops sfs_vfs_ops(void);

#endif /* MAZU_SFS_H */
