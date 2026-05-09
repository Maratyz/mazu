/* SPDX-License-Identifier: MIT */
#include "archive.h"
#include <mazu/ramfs.h>

static void mark_readonly_recursive(struct ram_fs_node *node)
{
    if (!node)
        return;
    node->flags |= RAM_FS_FLAG_READONLY;
    if (node->type == RAM_FS_TYPE_DIR) {
        for (struct ram_fs_node *child = node->first; child;
             child = child->next)
            mark_readonly_recursive(child);
    }
}

static u64 djb2_hash(struct byte_view bv)
{
    u64 hash = 5381;
    for (sz i = 0; i < bv.len; i++)
        hash = ((hash << 5) + hash) + bv.dat[i];
    return hash;
}

static u32 fnv1a32(struct byte_view bv)
{
    u32 hash = 2166136261u;
    for (sz i = 0; i < bv.len; i++) {
        hash ^= bv.dat[i];
        hash *= 16777619u;
    }
    return hash;
}

struct result archive_extract(struct byte_view archive, struct ram_fs *rfs)
{
    assert(rfs);

    if (archive.len < sizeof(struct ar_header))
        return result_error(EINVAL);

    /* Cast directly from the archive bytes because the archive uses
     * little-endian encoding matching the target (RISC-V 64-bit / x86-64).
     * This assumption is architecture-specific.
     */
    struct ar_header *header = (struct ar_header *) archive.dat;

    if (!str_is_equal(str_new(header->magic, countof(header->magic)),
                      MAGIC_STRING))
        return result_error(EINVAL);

    if (header->size < 0 || header->index_length < 0 ||
        header->size > archive.len)
        return result_error(EINVAL);

    sz index_offset = sizeof(struct ar_header);

    for (i64 i = 0; i < header->index_length; i++) {
        if (ADD_OVERFLOW((sz) archive.dat, index_offset))
            return result_error(EINVAL);
        if (index_offset + sizeof(struct ar_index_ent) > archive.len)
            return result_error(EINVAL);
        struct ar_index_ent *index_ent =
            (struct ar_index_ent *) (archive.dat + index_offset);

        if (index_ent->offset < 0 || index_ent->size < 0 ||
            index_ent->path_length < 0)
            return result_error(EINVAL);

        /* Validate file-data bounds:
         * 1) path_length must fit in entry size.
         * 2) data_len = size - path_length must not underflow.
         * 3) address and range arithmetic must not overflow and must fit in
         * archive.
         */
        if (index_ent->path_length > index_ent->size ||
            SUB_OVERFLOW(index_ent->size, index_ent->path_length) ||
            ADD_OVERFLOW((sz) archive.dat, (sz) index_ent->offset) ||
            ADD_OVERFLOW((sz) index_ent->offset, (sz) index_ent->size) ||
            (sz) index_ent->offset + (sz) index_ent->size > archive.len)
            return result_error(EINVAL);

        struct str path = str_new((char *) (archive.dat + index_ent->offset),
                                  index_ent->path_length);
        struct byte_view data = byte_view_new(
            archive.dat + index_ent->offset + index_ent->path_length,
            index_ent->size - index_ent->path_length);
        struct byte_view path_and_data =
            byte_view_new(archive.dat + index_ent->offset, index_ent->size);

        if (djb2_hash(path_and_data) != index_ent->hash)
            return result_error(EINVAL);

        struct result_ram_fs_node node_res =
            ram_fs_create_file(rfs->root, path, true);
        if (node_res.is_error)
            return result_error(node_res.code);

        struct ram_fs_node *node = result_ram_fs_node_checked(node_res);
        struct result_sz wr = ram_fs_write(node, data, 0);
        if (wr.is_error)
            return result_error(wr.code);

        /* Archive-backed content is immutable, so compute the ETag once when
         * extracting it.
         */
        node->etag = fnv1a32(data);

        if (ADD_OVERFLOW(index_offset, sizeof(struct ar_index_ent)))
            return result_error(EINVAL);
        index_offset += sizeof(struct ar_index_ent);
    }

    /* Mark all archive content (files and directories) as read-only. */
    mark_readonly_recursive(rfs->root);

    return result_ok();
}
