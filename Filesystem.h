#ifndef inode_h
#define inode_h

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <sys/stat.h>

#define Split '/'
#define NBuckets 1001

#define isDir(node) S_ISDIR((node)->mode)
#define isFile(node) S_ISREG((node)->mode)

typedef uint32_t uint;

typedef struct inode {
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int nlink;
    uint nopen;
    uint size;
    void* data;
    struct inode* parent;
    bool traversing; /// flag to avoid loops when releasing memory
} inode;

typedef struct entryTM { // apparently name `struct entry` is taken by some stupid search header
    char name[NAME_MAX];
    inode* node;
    struct entryTM* next; /// for linked list
} entry;

typedef struct {
    inode* root;
//    inode* table[NBuckets];
} Filesystem;

entry* addEntry(entry* list, const char* name);

inode* addNode(const char* path, inode* root, inode* node);
inode* moveNode(const char* path, const char* newpath, inode* root);
inode* pathfind(const char* path, inode* root);

bool releaseNode(const char* path, inode* root);

Filesystem* newFilesystem(void);

void releaseAll(inode* root);


#endif /* inode_h */
