/* SPDX-License-Identifier: MIT */
#include <mazu/kvalloc.h>
#include <mazu/ramfs.h>
#include <mazu/selftest.h>

#define RAM_FS_TEST_SIZE (4 * BIT(20)) /* 4MiB */

static u8 kvalloc_sentinel; /* non-NULL a_ptr for alloc_new assertion */
static struct alloc test_helper_create_alloc(struct arena *arn __unused)
{
    return alloc_new(&kvalloc_sentinel, kvalloc_alloc_wrapper,
                     kvalloc_free_wrapper);
}

static void test_path_name_parse(struct arena arn)
{
    struct result_path_name path_name_res;
    struct path_name path_name;

    path_name_res = path_name_parse(STR("/"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 0);
    assert(str_is_equal(path_name.src, STR("")));

    path_name_res = path_name_parse(STR("/this-is-random-nonsense"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 1);
    assert(str_is_equal(path_name.src, STR("this-is-random-nonsense")));
    assert(str_is_equal(path_name.components[0], STR("this-is-random-"
                                                     "nonsense")));

    /* Note the trailing '/' this time. */
    path_name_res = path_name_parse(STR("/this-is-random-nonsense/"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 1);
    assert(str_is_equal(path_name.src, STR("this-is-random-nonsense")));
    assert(str_is_equal(path_name.components[0], STR("this-is-random-"
                                                     "nonsense")));

    path_name_res = path_name_parse(STR("/foo/bar"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 2);
    assert(str_is_equal(path_name.src, STR("foo/bar")));
    assert(str_is_equal(path_name.components[0], STR("foo")));
    assert(str_is_equal(path_name.components[1], STR("bar")));

    /* Note the trailing '/' this time. */
    path_name_res = path_name_parse(STR("/foo/bar/"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 2);
    assert(str_is_equal(path_name.src, STR("foo/bar")));
    assert(str_is_equal(path_name.components[0], STR("foo")));
    assert(str_is_equal(path_name.components[1], STR("bar")));

    path_name_res = path_name_parse(STR("/foo//bar"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 2);
    assert(str_is_equal(path_name.src, STR("foo//bar")));
    assert(str_is_equal(path_name.components[0], STR("foo")));
    assert(str_is_equal(path_name.components[1], STR("bar")));

    /* The "special" path components '.' and '..' should be treated the same as
     * any path name because they are handled by the lookup routines, not by the
     * path parser.
     */
    path_name_res = path_name_parse(STR("/./blah/../..//.../"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == 5);
    assert(str_is_equal(path_name.src, STR("./blah/../..//...")));
    assert(str_is_equal(path_name.components[0], STR(".")));
    assert(str_is_equal(path_name.components[1], STR("blah")));
    assert(str_is_equal(path_name.components[2], STR("..")));
    assert(str_is_equal(path_name.components[3], STR("..")));
    assert(str_is_equal(path_name.components[4], STR("...")));

    /* Error conditions */

    path_name_res = path_name_parse(STR(""), &arn);
    assert(path_name_res.is_error);
    assert(path_name_res.code == EINVAL);

    /* Relative path (no leading '/') must return EINVAL, not crash. */
    path_name_res = path_name_parse(STR("foo/bar"), &arn);
    assert(path_name_res.is_error);
    assert(path_name_res.code == EINVAL);

    /* '\0' character is forbidden. */
    path_name_res = path_name_parse(STR("/blah/\0/foo"), &arn);
    assert(path_name_res.is_error);
    assert(path_name_res.code == EINVAL);

    /* Maximum length */
    struct str_buf sbuf = str_buf_from_arena(&arn, PATH_NAME_MAX_LEN + 2);
    for (i32 i = 0; i < PATH_NAME_MAX_LEN / 2; i++)
        str_buf_append(&sbuf, STR("/a"));
    path_name_res = path_name_parse(str_from_buf(sbuf), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(path_name.n_components == PATH_NAME_MAX_LEN / 2);
    for (i32 i = 0; i < PATH_NAME_MAX_LEN / 2; i++)
        assert(str_is_equal(path_name.components[i], STR("a")));

    /* Now make it too long */
    str_buf_append(&sbuf, STR("/a"));
    path_name_res = path_name_parse(str_from_buf(sbuf), &arn);
    assert(path_name_res.is_error);
    assert(path_name_res.code == ENAMETOOLONG);
}

static void test_path_name_to_str(struct arena arn)
{
    struct result_path_name path_name_res;
    struct path_name path_name;

    path_name_res = path_name_parse(STR("/"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(str_is_equal(path_name_to_str(path_name, &arn), STR("/")));

    path_name_res = path_name_parse(STR("/foo/bar"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(str_is_equal(path_name_to_str(path_name, &arn), STR("/foo/bar")));

    path_name_res = path_name_parse(STR("/foo//bar"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(str_is_equal(path_name_to_str(path_name, &arn), STR("/foo/bar")));

    path_name_res = path_name_parse(STR("/./blah/../..//.../"), &arn);
    assert(!path_name_res.is_error);
    path_name = result_path_name_checked(path_name_res);
    assert(str_is_equal(path_name_to_str(path_name, &arn), STR("/./blah/../../"
                                                               "...")));
}

static void test_ram_fs_node_lookup(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct ram_fs_node *root_dir =
        arena_alloc_aligned(&arn, sizeof(*root_dir), alignof(*root_dir));
    struct ram_fs_node *blah_dir =
        arena_alloc_aligned(&arn, sizeof(*blah_dir), alignof(*blah_dir));
    struct ram_fs_node *foo_file =
        arena_alloc_aligned(&arn, sizeof(*foo_file), alignof(*foo_file));
    struct ram_fs_node *bar_file =
        arena_alloc_aligned(&arn, sizeof(*bar_file), alignof(*bar_file));

    root_dir->first = NULL;
    root_dir->next = NULL;
    root_dir->type = RAM_FS_TYPE_DIR;
    root_dir->name = STR("");
    root_dir->data = byte_buf_new(NULL, 0, 0);
    root_dir->fs = rfs;

    blah_dir->first = NULL;
    blah_dir->next = NULL;
    blah_dir->type = RAM_FS_TYPE_DIR;
    blah_dir->name = STR("blah");
    blah_dir->data = byte_buf_new(NULL, 0, 0);
    blah_dir->fs = rfs;

    foo_file->first = NULL;
    foo_file->next = NULL;
    foo_file->type = RAM_FS_TYPE_FILE;
    foo_file->name = STR("foo");
    foo_file->data = byte_buf_new(NULL, 0, 0);
    foo_file->fs = rfs;

    bar_file->first = NULL;
    bar_file->next = NULL;
    bar_file->type = RAM_FS_TYPE_FILE;
    bar_file->name = STR("bar");
    bar_file->data = byte_buf_new(NULL, 0, 0);
    bar_file->fs = rfs;

    root_dir->first = blah_dir;
    blah_dir->first = foo_file;
    foo_file->next = bar_file;

    rfs->root = root_dir;

    assert(
        ram_fs_node_lookup(rfs->root, result_path_name_checked(path_name_parse(
                                          STR("/"), &arn))) == root_dir);
    assert(
        ram_fs_node_lookup(rfs->root, result_path_name_checked(path_name_parse(
                                          STR("/blah"), &arn))) == blah_dir);
    assert(ram_fs_node_lookup(rfs->root,
                              result_path_name_checked(path_name_parse(
                                  STR("/blah/foo"), &arn))) == foo_file);
    assert(ram_fs_node_lookup(rfs->root,
                              result_path_name_checked(path_name_parse(
                                  STR("/blah/bar"), &arn))) == bar_file);
}

static void test_ram_fs_create_dir(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct result_ram_fs_node dir_res;

    /* Create two directories /foo and /foo/bar */

    dir_res = ram_fs_create_dir(rfs->root, STR("/foo"), false);
    assert(!dir_res.is_error);
    struct ram_fs_node *foo_dir = result_ram_fs_node_checked(dir_res);
    assert(foo_dir->type == RAM_FS_TYPE_DIR);
    assert(str_is_equal(foo_dir->name, STR("foo")));

    dir_res = ram_fs_create_dir(rfs->root, STR("/foo/bar"), false);
    assert(!dir_res.is_error);
    struct ram_fs_node *bar_dir = result_ram_fs_node_checked(dir_res);
    assert(bar_dir->type == RAM_FS_TYPE_DIR);
    assert(str_is_equal(bar_dir->name, STR("bar")));

    assert(rfs->root->first == foo_dir);
    assert(foo_dir->first == bar_dir);

    /* Create another directory in /foo next to /foo/bar */
    dir_res = ram_fs_create_dir(rfs->root, STR("/foo/baz"), false);
    assert(!dir_res.is_error);
    struct ram_fs_node *baz_dir = result_ram_fs_node_checked(dir_res);
    assert(baz_dir->type == RAM_FS_TYPE_DIR);
    assert(str_is_equal(baz_dir->name, STR("baz")));

    assert(bar_dir->next == baz_dir);

    /* Can't create the same directory again */
    dir_res = ram_fs_create_dir(rfs->root, STR("/foo/bar"), false);
    assert(dir_res.is_error);
    assert(dir_res.code == EEXIST);

    /* Can't create root directory */
    dir_res = ram_fs_create_dir(rfs->root, STR("/"), false);
    assert(dir_res.is_error);
    assert(dir_res.code == EEXIST);

    /* Can't create directory without parent directory */
    dir_res =
        ram_fs_create_dir(rfs->root, STR("/this-doesn't-exist/bar/"), false);
    assert(dir_res.is_error);
    assert(dir_res.code == ENOENT);

    /* Recursive directory creation works */
    dir_res = ram_fs_create_dir(rfs->root,
                                STR("/this-doesn't-exist/beep/boop/"), true);
    assert(!dir_res.is_error);
    struct ram_fs_node *boop_dir = result_ram_fs_node_checked(dir_res);
    assert(boop_dir->type == RAM_FS_TYPE_DIR);
    assert(str_is_equal(boop_dir->name, STR("boop")));

    /* Create a directory from a subdirectory instead of root */
    struct result_ram_fs_node res = ram_fs_open(rfs->root, STR("/foo"));
    assert(!res.is_error);
    struct ram_fs_node *foo_from_open = result_ram_fs_node_checked(res);

    dir_res = ram_fs_create_dir(foo_from_open, STR("/subdir"), false);
    assert(!dir_res.is_error);
    struct ram_fs_node *subdir = result_ram_fs_node_checked(dir_res);
    assert(str_is_equal(subdir->name, STR("subdir")));
    assert(foo_from_open->first->next->next == subdir); /* after bar and baz */
}

static void test_ram_fs_create_file(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct result_ram_fs_node dir_res;
    struct result_ram_fs_node file_res;

    /* Create a parent directory /foo for the files */
    dir_res = ram_fs_create_dir(rfs->root, STR("/foo"), false);
    assert(!dir_res.is_error);
    struct ram_fs_node *foo_dir = result_ram_fs_node_checked(dir_res);
    assert(foo_dir->type == RAM_FS_TYPE_DIR);
    assert(str_is_equal(foo_dir->name, STR("foo")));

    /* Create a file /foo/bar.txt and verify its correctness */
    file_res = ram_fs_create_file(rfs->root, STR("/foo/bar.txt"), false);
    assert(!file_res.is_error);
    struct ram_fs_node *bar_file = result_ram_fs_node_checked(file_res);
    assert(bar_file->type == RAM_FS_TYPE_FILE);
    assert(str_is_equal(bar_file->name, STR("bar.txt")));

    /* Attempt to create the same file again */
    file_res = ram_fs_create_file(rfs->root, STR("/foo/bar.txt"), false);
    assert(file_res.is_error);
    assert(file_res.code == EEXIST);

    /* Create another file in the same directory and verify links are correct.
     */
    file_res = ram_fs_create_file(rfs->root, STR("/foo/baz.txt"), false);
    assert(!file_res.is_error);
    struct ram_fs_node *baz_file = result_ram_fs_node_checked(file_res);
    assert(baz_file->type == RAM_FS_TYPE_FILE);
    assert(str_is_equal(baz_file->name, STR("baz.txt")));

    assert(foo_dir->first == bar_file);
    assert(bar_file->next == baz_file);
    assert(baz_file->next == NULL);

    /* Attempt to create a file in a non-existent parent directory */
    file_res =
        ram_fs_create_file(rfs->root, STR("/nonexistent/dir/file.txt"), false);
    assert(file_res.is_error);
    assert(file_res.code == ENOENT);

    /* Recursive file creation works */
    file_res =
        ram_fs_create_file(rfs->root, STR("/nonexistent/dir/file.txt"), true);
    assert(!file_res.is_error);
    struct ram_fs_node *rec_file = result_ram_fs_node_checked(file_res);
    assert(rec_file->type == RAM_FS_TYPE_FILE);
    assert(str_is_equal(rec_file->name, STR("file.txt")));

    /* Attempt to create a file inside another file */
    file_res =
        ram_fs_create_file(rfs->root, STR("/foo/bar.txt/subfile"), false);
    assert(file_res.is_error);
    assert(file_res.code == ENOTDIR);

    /* Recursive file creation doesn't work if the parent is a file */
    file_res = ram_fs_create_file(rfs->root, STR("/foo/bar.txt/subfile"), true);
    assert(file_res.is_error);
    assert(file_res.code == ENOTDIR);

    /* Trailing '/' is accepted because ram_fs_create_file clearly
     * expresses the intent of creating a file.
     */
    file_res =
        ram_fs_create_file(rfs->root, STR("/foo/trailing_slash/"), false);
    assert(!file_res.is_error);

    /* Create a file from a subdirectory instead of root */
    struct result_ram_fs_node res = ram_fs_open(rfs->root, STR("/foo"));
    assert(!res.is_error);
    struct ram_fs_node *foo_from_open = result_ram_fs_node_checked(res);

    file_res = ram_fs_create_file(foo_from_open, STR("/nested.txt"), false);
    assert(!file_res.is_error);
    struct ram_fs_node *nested_file = result_ram_fs_node_checked(file_res);
    assert(str_is_equal(nested_file->name, STR("nested.txt")));
}

static void test_ram_fs_open(struct arena arn)
{
    struct arena arn_cpy = arn; /* copy so we can create fresh ram_fs twice */
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn_cpy));

    /* Manually set up the filesystem structure to avoid dependency on
     * file creation logic.
     */
    struct ram_fs_node *dir_node =
        arena_alloc_aligned(&arn_cpy, sizeof(*dir_node), alignof(*dir_node));
    struct ram_fs_node *file_node =
        arena_alloc_aligned(&arn_cpy, sizeof(*file_node), alignof(*file_node));

    dir_node->first = NULL;
    dir_node->next = NULL;
    dir_node->type = RAM_FS_TYPE_DIR;
    dir_node->name = STR("dir");
    dir_node->data = byte_buf_new(NULL, 0, 0);
    dir_node->fs = rfs;

    file_node->first = NULL;
    file_node->next = NULL;
    file_node->type = RAM_FS_TYPE_FILE;
    file_node->name = STR("file");
    file_node->data = byte_buf_new(NULL, 0, 0);
    file_node->fs = rfs;

    rfs->root->first = dir_node;
    dir_node->next = file_node;

    struct result_ram_fs_node res;

    /* Root path returns root node */
    res = ram_fs_open(rfs->root, STR("/"));
    assert(!res.is_error);
    assert(result_ram_fs_node_checked(res) == rfs->root);

    /* Valid directory path returns directory node */
    res = ram_fs_open(rfs->root, STR("/dir"));
    assert(!res.is_error);
    assert(result_ram_fs_node_checked(res) == dir_node);

    /* Valid file path returns file node */
    res = ram_fs_open(rfs->root, STR("/file"));
    assert(!res.is_error);
    assert(result_ram_fs_node_checked(res) == file_node);

    /* Non-existent path returns ENOENT error */
    res = ram_fs_open(rfs->root, STR("/invalid"));
    assert(res.is_error);
    assert(res.code == ENOENT);

    /* Trailing slash on directory path is handled correctly */
    res = ram_fs_open(rfs->root, STR("/dir/"));
    assert(!res.is_error);
    assert(result_ram_fs_node_checked(res) == dir_node);

    /* Consecutive slashes result in ENOENT error */
    res = ram_fs_open(rfs->root, STR("/dir//file"));
    assert(res.is_error);
    assert(res.code == ENOENT);

    /* Can't use file as intermediate path */
    res = ram_fs_open(rfs->root, STR("/file/dir"));
    assert(res.is_error);
    assert(res.code == ENOENT);

    /* Path lookup in empty filesystem returns ENOENT error */
    arn_cpy = arn;
    struct ram_fs *empty_rfs = ram_fs_new(test_helper_create_alloc(&arn_cpy));
    res = ram_fs_open(empty_rfs->root, STR("/dir"));
    assert(res.is_error);
    assert(res.code == ENOENT);

    /* Case sensitivity results in ENOENT error for mismatched case */
    res = ram_fs_open(rfs->root, STR("/DIR"));
    assert(res.is_error);
    assert(res.code == ENOENT);

    /* Tests for opening from a subdirectory instead of root: */
    /* Create a file in the subdirectory first */
    struct result_ram_fs_node create_res =
        ram_fs_create_file(dir_node, STR("/subfile"), false);
    assert(!create_res.is_error);

    /* Now try to open it from the subdirectory */
    res = ram_fs_open(dir_node, STR("/subfile"));
    assert(!res.is_error);
    struct ram_fs_node *subfile = result_ram_fs_node_checked(res);
    assert(str_is_equal(subfile->name, STR("subfile")));

    /* Verify it doesn't exist when opening from root */
    res = ram_fs_open(rfs->root, STR("/subfile"));
    assert(res.is_error);
    assert(res.code == ENOENT);
}

static void test_ram_fs_read(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct result_sz res;
    struct byte_buf bbuf;

    /* Manually set up the filesystem structure to avoid dependency on
     * file creation logic.
     */
    struct ram_fs_node *file_node =
        arena_alloc_aligned(&arn, sizeof(*file_node), alignof(*file_node));
    file_node->first = NULL;
    file_node->next = NULL;
    file_node->type = RAM_FS_TYPE_FILE;
    file_node->name = STR("file");
    file_node->data = byte_buf_from_array(byte_array_from_arena(13, &arn));
    file_node->fs = rfs;
    byte_buf_append(&file_node->data, byte_view_from_str(STR("Hello, world!")));

    rfs->root->first = file_node;

    /* Read the entire file */
    bbuf = byte_buf_from_array(byte_array_from_arena(13, &arn));
    res = ram_fs_read(file_node, &bbuf, 0);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 13);
    assert(byte_view_is_equal(byte_view_from_buf(bbuf),
                              byte_view_from_str(STR("Hello, world!"))));

    /* Read a part of the file */
    bbuf = byte_buf_from_array(byte_array_from_arena(5, &arn));
    res = ram_fs_read(file_node, &bbuf, 7);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 5);
    assert(byte_view_is_equal(byte_view_from_buf(bbuf),
                              byte_view_from_str(STR("world"))));

    /* Read past the end of the file */
    bbuf = byte_buf_from_array(byte_array_from_arena(5, &arn));
    res = ram_fs_read(file_node, &bbuf, 13);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 0);
    assert(byte_view_is_equal(byte_view_from_buf(bbuf),
                              byte_view_from_str(STR(""))));

    /* Read with offset past the end of the file */
    bbuf = byte_buf_from_array(byte_array_from_arena(5, &arn));
    res = ram_fs_read(file_node, &bbuf, 14);
    assert(res.is_error);
    assert(res.code == EINVAL);

    /* Read with offset past the end of the file */
    bbuf = byte_buf_from_array(byte_array_from_arena(5, &arn));
    res = ram_fs_read(file_node, &bbuf, 15);
    assert(res.is_error);
    assert(res.code == EINVAL);

    /* Reject reading from a directory */
    struct ram_fs_node *dir_node =
        arena_alloc_aligned(&arn, sizeof(*dir_node), alignof(*dir_node));
    dir_node->first = NULL;
    dir_node->next = NULL;
    dir_node->type = RAM_FS_TYPE_DIR;
    dir_node->name = STR("dir");
    dir_node->data = byte_buf_new(NULL, 0, 0);
    dir_node->fs = rfs;

    bbuf = byte_buf_from_array(byte_array_from_arena(5, &arn));
    res = ram_fs_read(dir_node, &bbuf, 0);
    assert(res.is_error);
    assert(res.code == EINVAL);
}

static void test_ram_fs_write(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct result_sz res;

    /* Manually set up the filesystem structure to avoid dependency on
     * file creation logic.
     */
    struct ram_fs_node *file_node =
        arena_alloc_aligned(&arn, sizeof(*file_node), alignof(*file_node));
    file_node->first = NULL;
    file_node->next = NULL;
    file_node->type = RAM_FS_TYPE_FILE;
    file_node->name = STR("file");
    file_node->fs = rfs;
    void *data = alloc_alloc(rfs->data_alloc, 13, alignof(void *));
    assert(data);
    file_node->data = byte_buf_new(data, 0, 13);

    rfs->root->first = file_node;

    /* Write to an empty file */
    res = ram_fs_write(file_node, byte_view_from_str(STR("Hello, world!")), 0);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 13);
    assert(byte_view_is_equal(byte_view_from_buf(file_node->data),
                              byte_view_from_str(STR("Hello, world!"))));

    /* Write to the beginning of the file */
    res = ram_fs_write(file_node, byte_view_from_str(STR("Adieu, ")), 0);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 7);
    assert(byte_view_is_equal(byte_view_from_buf(file_node->data),
                              byte_view_from_str(STR("Adieu, world!"))));

    /* Write to the end of the file */
    res = ram_fs_write(file_node, byte_view_from_str(STR("!!!")), 13);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 3);
    assert(byte_view_is_equal(byte_view_from_buf(file_node->data),
                              byte_view_from_str(STR("Adieu, world!!!!"))));

    /* Write to the middle of the file */
    res = ram_fs_write(file_node, byte_view_from_str(STR("friend")), 7);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 6);
    assert(byte_view_is_equal(byte_view_from_buf(file_node->data),
                              byte_view_from_str(STR("Adieu, friend!!!"))));

    /* Write with offset past the end of the file */
    res = ram_fs_write(file_node, byte_view_from_str(STR("!!!")), 21);
    assert(res.is_error);
    assert(res.code == EINVAL);

    /* Write with an offset close to the end so part of the write
     * exceeds the file boundary.
     */
    res = ram_fs_write(file_node, byte_view_from_str(STR("......")), 13);
    assert(!res.is_error);
    assert(result_sz_checked(res) == 6);
    assert(byte_view_is_equal(byte_view_from_buf(file_node->data),
                              byte_view_from_str(STR("Adieu, friend......"))));
}

static void test_ram_fs_e2e(struct arena arn)
{
    struct ram_fs *rfs = ram_fs_new(test_helper_create_alloc(&arn));

    struct result_ram_fs_node res;
    struct result_sz res_sz;

    /* Create a directory /foo and a file /foo/bar.txt */
    res = ram_fs_create_dir(rfs->root, STR("/foo"), false);
    assert(!res.is_error);

    res = ram_fs_create_file(rfs->root, STR("/foo/bar.txt"), false);
    assert(!res.is_error);
    struct ram_fs_node *bar_file = result_ram_fs_node_checked(res);

    /* Write to bar_file */
    res_sz = ram_fs_write(bar_file, byte_view_from_str(STR("Blah")), 0);
    assert(!res_sz.is_error);
    assert(result_sz_checked(res_sz) == 4);

    /* Open the file again and write to it */
    res = ram_fs_open(rfs->root, STR("/foo/bar.txt"));
    assert(!res.is_error);
    struct ram_fs_node *bar_file_opened = result_ram_fs_node_checked(res);

    res_sz = ram_fs_write(bar_file_opened,
                          byte_view_from_str(STR("Hello, world!")), 0);
    assert(!res_sz.is_error);
    assert(result_sz_checked(res_sz) == 13);

    /* Open the file and read from it */
    res = ram_fs_open(rfs->root, STR("/foo/bar.txt"));
    assert(!res.is_error);
    bar_file_opened = result_ram_fs_node_checked(res);

    struct byte_buf bbuf = byte_buf_from_array(byte_array_from_arena(13, &arn));
    res_sz = ram_fs_read(bar_file_opened, &bbuf, 0);
    assert(!res_sz.is_error);
    assert(result_sz_checked(res_sz) == 13);
    assert(byte_view_is_equal(byte_view_from_buf(bbuf),
                              byte_view_from_str(STR("Hello, world!"))));
}

static void ram_fs_run_tests(struct arena arn)
{
    test_path_name_parse(arn);
    test_path_name_to_str(arn);
    test_ram_fs_node_lookup(arn);
    test_ram_fs_create_dir(arn);
    test_ram_fs_create_file(arn);
    test_ram_fs_open(arn);
    test_ram_fs_read(arn);
    test_ram_fs_write(arn);
    test_ram_fs_e2e(arn);
}

static i32 test_ramfs(void)
{
    struct byte_array tmp_ba =
        option_byte_array_checked(kvalloc_alloc(5 * BIT(20), alignof(void *)));
    struct arena tmp = arena_new(tmp_ba);
    ram_fs_run_tests(tmp);
    kvalloc_free(tmp_ba);
    return 0;
}
DEFINE_SELFTEST(ramfs, test_ramfs);
