/* SPDX-License-Identifier: MIT */
#ifndef MAZU_ARCHIVE_H
#define MAZU_ARCHIVE_H

#include <mazu/base.h>
#include <mazu/byte.h>
#include <mazu/error.h>
#include <mazu/ramfs.h>

#define MAGIC_SIZE 8
#define MAGIC_STRING STR("MAZU__AR")

/* This archive file format uses little endian encoding. */

struct ar_header {
    char magic[MAGIC_SIZE]; /* Magic number should contain MAGIC_STRING. */
    i64 index_length;       /* Number of entries in the index. */
    i64 size;               /* Total size of the archive including the header,
                             * the index and the file data.
                             */
} __packed;

struct ar_index_ent {
    u64 hash;        /* Hash of the file path prepended to the file data.
                      * Uses DJB2 hash algorithm.
                      */
    i64 offset;      /* Offset of the first byte of the file entry with respect
                      * to the beginning of the archive.
                      */
    i64 size;        /* Size of the file path plus file data. */
    i64 path_length; /* Length of the file path in bytes. */
    u32 flags;       /* Unused. Could be used for permissions later. */
} __packed;

/* A file entry would look like this:
 *
 *   struct ar_file_ent {
 *       char path[path_length];
 *       byte data[size - path_length];
 *   };
 */

/* Extracts the archive 'archive' into the given FS. */
struct result archive_extract(struct byte_view archive, struct ram_fs *rfs);

#endif /* MAZU_ARCHIVE_H */
