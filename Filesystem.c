#include "Filesystem.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

static int checkPath(const char* path);
static int extractPrefix(char* result, const char* path);
static bool isEmpty(inode* dir);
static inode* getParentDirectory(const char* path, const char** name, inode* root);

/// Traverse directories starting from `root` according to `path`
/// - Returns: `NULL` if search failed, pointer to the found `indoe` otherwise
inode* pathfind(const char* path, inode* root) {
    if (*path == Split) path++; // optional / at the start
    if (!*path) return root;

    if (!isDir(root)) {
        fprintf(stderr, "Name previous to '%s', is not a directory\n", path);
        errno = ENOTDIR;
        return NULL;
    }

    int n = 0;
    for (; path[n] != '\0' && path[n] != Split; ++n);

    for (entry* p = root->data; p; p = p->next)
        if (strlen(p->name) == n && strncmp(path, p->name, n) == 0)
            return pathfind(path + n, p->node);

    errno = ENOENT;
    return NULL;
}

/// Allocates new entry in the `list` with `name`
/// - Returns the pointer to it, or `NULL` if error occurs
entry* addEntry(entry* list, const char* name) {
    while (list->next) list = list->next;
    list->next = calloc(1, sizeof(entry));
    entry* result = list->next;
    if (!result) {
        errno = ENOSPC;
        return NULL;
    }

    strcpy(result->name, name);

    return result;
}

/// Deletes `name`d entry in the `list`
/// - Returns: if entry was found, pointer to the corresponding inode, `NULL` otherwise
inode* removeEntry(entry* list, const char* name) {
    for (entry* p = list; p->next; p = p->next)
        if (strcmp(name, p->next->name) == 0) {
            inode* result = p->next->node;
            entry* old = p->next;
            p->next = p->next->next;
            free(old);
            return result;
        }
    errno = ENOENT;
    return NULL;
}

#define errorFree(call) if ((call) < 0) return NULL;

/// Adds `node` to the `path` location, starting from `root` inode
/// - Returns: pointer to the added inode (just in case), `NULL` on error
inode* addNode(const char* path, inode* root, inode* node) {
    const char* file;
    inode* dirNode = getParentDirectory(path, &file, root);
    if (!dirNode) return NULL;

    if (pathfind(file, dirNode)) {
        errno = EEXIST;
        return NULL;
    }

    entry* p = addEntry(dirNode->data, file);
    if (!p) return NULL;
    
    node->nlink++;
    if (!node->parent) node->parent = dirNode;

    p->node = node;

    return node;
}

inode* moveNode(const char* path, const char* newpath, inode* root) {
    const char *oldFile, *newFile;
    inode* oldDirNode = getParentDirectory(path, &oldFile, root);
    if (!oldDirNode) return NULL;
    inode* newDirNode = getParentDirectory(newpath, &newFile, root);
    if (!newDirNode) return NULL;

    inode* node = removeEntry(oldDirNode->data, oldFile);
    if (!node) return NULL;

    entry* p = addEntry(newDirNode->data, newFile);
    if (!p) return NULL;

    p->node = node;

    return node;
}

/// Unlinks an inode from `path`, if 0 links left, deletes the inode, either regular file or an empty directory
/// - Returns: `true` if successful, `false` if error
bool releaseNode(const char* path, inode* root) {
    const char* file;
    inode* dirNode = getParentDirectory(path, &file, root);
    if (!dirNode) return NULL;

    inode* node = pathfind(file, dirNode);

    // if it is an empty directory, remove .. to correctly free the list
    if (isDir(node)) {
        if (!isEmpty(node)) {
            errno = ENOTEMPTY;
            return false;
        }
        assert(node->nlink == 2); // . and itself
        removeEntry(node->data, "..");
        dirNode->nlink--; // cause .. was referencing it
        assert(((entry*)node->data)->next == NULL);
        free(node->data);
        free(node);
    } else if (--node->nlink == 0 && !node->nopen) {
        // file needs to be closed and with 0 links
        if (node->data) free(node->data);
        free(node);
    }

    node = removeEntry(dirNode->data, file);
    if (!node) return false;

    return true;
}

Filesystem* newFilesystem(void) {
    Filesystem* fs = malloc(sizeof(Filesystem));
    inode* root = calloc(1, sizeof(inode));
    root->mode = S_IRWXO | S_IRWXG | S_IRWXU | S_IFDIR;
    root->nlink = 1;
    root->parent = root;

    entry* ddot = malloc(sizeof(entry));
    strcpy(ddot->name, "..");
    ddot->node = root;
    root->nlink++;
    ddot->next = NULL;

    entry* dot = malloc(sizeof(entry));
    strcpy(dot->name, ".");
    dot->node = root;
    root->nlink++;
    dot->next = ddot;

    root->data = dot;
    fs->root = root;

    return fs;
}

void releaseAll(inode* root) {
    root->nlink--;
    if (root->traversing) return;
    root->traversing = true;

    if (root->nopen) fprintf(stderr, "Warning: releasing an open file\n");

    if (isDir(root)) {
        entry* p = root->data;
        while (p->next) {
            releaseAll(p->next->node);
            entry* old = p->next;
            p->next = p->next->next;
            free(old);
        }
        releaseAll(p->node);
    }

    root->traversing = false;
    if (root->nlink == 0) {
        if (root->data) free(root->data);
        free(root);
    }
}

static int checkPath(const char* path) {
    if (!*path) {
        errno = ENOENT;
        return -1;
    }
    if (*path != Split) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int extractPrefix(char* result, const char* path) {
    strcpy(result, path);

    int n = (int)strlen(result);
    while (result[--n] != Split) assert(n >= 0);
    result[n] = '\0';

    if (strlen(result) - n == 1) {
        fprintf(stderr, "filename for '%s' is empty", path);
        errno = ENOENT;
        return -1;
    }

    return n;
}

/// Checks if directory is empty (if only . and .. are its members)
static bool isEmpty(inode* dir) {
    int n = 0;
    for (entry* p = dir->data; p; p = p->next)
        if (++n > 2) return false;
    assert(n == 2); // can't be less
    return true;
}

/// Finds parent directory for the file with `path`, sets `name` to point to the start of the filename in `path`
static inode* getParentDirectory(const char* path, const char** name, inode* root) {
    int n;
    char dirPath[PATH_MAX];
    errorFree(checkPath(path));
    errorFree(n = extractPrefix(dirPath, path));

    inode* dirNode = pathfind(dirPath, root);
    if (!dirNode) return NULL;

    if (!isDir(dirNode)) {
        errno = ENOTDIR;
        return NULL;
    }
    
    // "dirPath / [path + n + 1]: fileName \0"
    if (name) *name = path + n + 1;
    return dirNode;
}
