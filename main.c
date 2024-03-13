#define FUSE_USE_VERSION 26

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fuse.h>
#include <string.h>

#include "Filesystem.h"

/** Get file attributes.
 Similar to stat().  The `st_dev` and `st_blksize` fields are
 ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 mount option is given.
*/
int ramGetattr(const char *path, struct stat *statbuf) {
    Filesystem* fs = fuse_get_context()->private_data;

    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    statbuf->st_mode = node->mode;
    statbuf->st_uid = node->uid;
    statbuf->st_gid = node->gid;
    statbuf->st_nlink = node->nlink;
    statbuf->st_size = node->size;

    return 0;
}

/** Create a file node
 There is no create() operation, mknod() will be called for
 creation of all non-directory, non-symlink nodes.
*/
// shouldn't that comment be "if" there is no.... ?
int ramMknod(const char *path, mode_t mode, dev_t dev) {
    struct fuse_context* ctx = fuse_get_context();
    Filesystem* fs = ctx->private_data;

    inode* node = calloc(1, sizeof(inode));
    if (!node) return -ENOSPC;
    node->mode = mode | S_IFREG;
    node->uid = ctx->uid;
    node->gid = ctx->gid;

    node = addNode(path, fs->root, node);
    if (!node) return -errno;

    return 0;
}

/// Create a directory
int ramMkdir(const char *path, mode_t mode) {
    struct fuse_context* ctx = fuse_get_context();
    Filesystem* fs = ctx->private_data;

    inode* node = calloc(1, sizeof(inode));
    if (!node) return -ENOSPC;
    node->mode = mode | S_IFDIR;
    node->uid = ctx->uid;
    node->gid = ctx->gid;

    node = addNode(path, fs->root, node);
    if (!node) return -errno;

    entry* ddot = malloc(sizeof(entry));
    strcpy(ddot->name, "..");
    ddot->node = node->parent;
    node->parent->nlink++;
    ddot->next = NULL;

    entry* dot = malloc(sizeof(entry));
    strcpy(dot->name, ".");
    dot->node = node;
    node->nlink++;
    dot->next = ddot;

    node->data = dot;

    return 0;
}

/// Create a hard link to a file
int ramLink(const char *path, const char *newpath) {
    Filesystem* fs = fuse_get_context()->private_data;

    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    if (isDir(node)) return -EPERM;

    node = addNode(newpath, fs->root, node);
    if (!node) return -errno;

    return 0;
}

/// Remove a file
int ramUnlink(const char *path) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    if (isDir(node)) return -EINVAL;
    if (node->nopen) return -EBUSY;

    bool ok = releaseNode(path, fs->root);
    if (!ok) return -errno;

    return 0;
}

/** Open directory
 This method should check if the open operation is permitted for
 this  directory
*/
int ramOpendir(const char *path, struct fuse_file_info *fi) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;
    if (!isDir(node)) return -ENOTDIR;
    fi->fh = (uint64_t)node;
    return 0;
}


/** Remove a directory */
int ramRmdir(const char *path) {
    if (strcmp(path, "/") == 0) return -EBUSY; // mount point
    Filesystem* fs = fuse_get_context()->private_data;

    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    if (!isDir(node)) return -ENOTDIR;

    bool ok = releaseNode(path, fs->root);
    if (!ok) return -errno;

    return 0;
}

/** Read directory

 This supersedes the old getdir() interface.  New applications
 should use this.

 The readdir implementation ignores the offset parameter, and
 passes zero to the filler function's offset.  The filler
 function will not return '1' (unless an error happens), so the
 whole directory is read in a single readdir operation.  This
 works just like the old getdir() method.
 */
int ramReaddir(const char *path, void *buf, fuse_fill_dir_t filler,
               off_t offset, struct fuse_file_info *fi) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    if (!isDir(node)) return -ENOTDIR;

    for (entry* p = node->data; p; p = p->next)
        filler(buf, p->name, NULL, 0);

    return 0;
}

/// Release directory (does absolutely nothing)
int ramReleasedir(const char *path, struct fuse_file_info *fi) {
    fi->fh = 0;
    return 0;
}

bool isValidRename(const char* path, const char* newpath) {
    size_t n = strlen(path);
    if (strncmp(path, newpath, n) == 0 && strlen(newpath) > n) {
        // path is a prefix of newpath
        if (newpath[n] == Split) return false;
    }
    // . or .. are not allowed in this
    if (strchr(newpath, '.') || strchr(path, '.'))
        return false;
    return true;
}

/// Rename a file
// both path and newpath are fs-relative
int ramRename(const char *path, const char *newpath) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;

    if (!isValidRename(path, newpath)) return -EINVAL;

    if ((node = pathfind(newpath, fs->root))) {
        if (isDir(node)) return -EISDIR; // will not recursively delete whole direcoty
        bool ok = releaseNode(newpath, fs->root);
        if (!ok) return -errno;
    }

    node = moveNode(path, newpath, fs->root);
    if (!node) return -errno;

    return 0;
}

/** File open operation

 No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 will be passed to open().  Open should check if the operation
 is permitted for the given flags.  Optionally open may also
 return an arbitrary filehandle in the fuse_file_info structure,
 which will be passed to all file operations.
*/
int ramOpen(const char *path, struct fuse_file_info *fi) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;
    if (isDir(node)) return -EISDIR;

    node->nopen++;
    fi->fh = (uint64_t)node;

    return 0;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
// I don't fully understand the documentation above -- it doesn't
// match the documentation for the read() system call which says it
// can return with anything up to the amount of data requested. nor
// with the fusexmp code which returns the amount of data also
// returned by read.
int ramRead(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode* node = (inode*)fi->fh;
    if (!node->nopen) return -EBADF;

    if (isDir(node)) return -EISDIR;
    if (offset + size > node->size) size = node->size; //hreturn -EINVAL;

    memcpy(buf, (char*)node->data + offset, size);

    return (int)size;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 */
// As  with read(), the documentation above is inconsistent with the
// documentation for the write() system call.
int ramWrite(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    inode* node = (inode*)fi->fh;
    if (!node->nopen) return -EBADF;

    if (isDir(node)) return -EISDIR;

    if (offset + size > node->size) {
        char* data = realloc(node->data, offset + size);
        if (!data) return -ENOSPC;
        node->data = data;
        node->size = (uint)(size + offset);
    }
    memcpy((char*)node->data + offset, buf, size);

    return (int)size;
}

int ramTruncate(const char* path, off_t offset) {
    Filesystem* fs = fuse_get_context()->private_data;
    inode* node = pathfind(path, fs->root);
    if (!node) return -errno;
    if (isDir(node)) return -EISDIR;

    if (offset == 0) {
        if (node->data) free(node->data);
        node->data = NULL;
    } else {
        char* data = realloc(node->data, offset);
        if (!data) return -ENOSPC;
        node->data = data;
    }
    if (offset > node->size)
        memset(node->data + node->size, 0, offset - node->size);
    node->size = (uint)offset;

    return 0;
}

/** Release an open file

 Release is called when there are no more references to an open
 file: all file descriptors are closed and all memory mappings
 are unmapped.

 For every open() call there will be exactly one release() call
 with the same flags and file descriptor.  It is possible to
 have a file opened more than once, in which case only the last
 release will mean, that no more reads/writes will happen on the
 file.  The return value of release is ignored.
*/
int ramRelease(const char *path, struct fuse_file_info *fi) {
    inode* node = (inode*)fi->fh;
    if (!node->nopen) return -EBADF;
    if (--node->nopen == 0 && node->nlink == 0) {
        Filesystem* fs = fuse_get_context()->private_data;
        bool ok = releaseNode(path, fs->root);
        if (!ok) return -errno;
    }
    return 0;
}

/**
 Initialize filesystem

 The return value will passed in the private_data field of
 fuse_context to all file operations and as a parameter to the
 destroy() method.
*/
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *ramInit(struct fuse_conn_info *conn) {
    Filesystem* fs = newFilesystem();
    fprintf(stderr, "Filesystem initialized\n");
    return fs;
}

/**
 Clean up filesystem

 Called on filesystem exit.
*/
void ramDestroy(void *userdata) {
    Filesystem* fs = fuse_get_context()->private_data;
    fprintf(stderr, "Destroying the filesystem\n");
    releaseAll(fs->root);
    free(fs);
}

struct fuse_operations operations = {
    .getattr = ramGetattr,
    // no .getdir -- that's deprecated
    .mknod = ramMknod,
    .mkdir = ramMkdir,
    .unlink = ramUnlink,
    .rmdir = ramRmdir,
    .rename = ramRename,
    .link = ramLink,
    .open = ramOpen,
    .read = ramRead,
    .write = ramWrite,
    .release = ramRelease,
    .truncate = ramTruncate,
    .opendir = ramOpendir,
    .readdir = ramReaddir,
    .releasedir = ramReleasedir,
    .init = ramInit,
    .destroy = ramDestroy
};

int main(int argc, char *argv[]) {
    // See which version of fuse we're running
    fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);

    // turn over control to fuse
    fprintf(stderr, "about to call fuse_main\n");
    int ok = fuse_main(argc, argv, &operations, NULL);
    fprintf(stderr, "fuse_main returned %d\n", ok);

    return ok;
}
