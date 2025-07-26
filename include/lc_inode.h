
#ifndef LC_INODE_H
#define LC_INODE_H

#include <cstdint>
#include <cstring>

#include "lc_memory.h"
#include "lc_utils.h"

#define LC_INODE_SIZE       256
#define LC_INODE_USED_SIZE  104
#define LC_DIRECT_PTRS      12
#define LC_BLOCK_PTRS_SIZE  15  // 12 direct + 3 indirect
#define LC_INODES_PRE_BLOCK 16  // 4096 / 256
#define LC_BLOCK_PTR_SIZE   4   // sizeof(uint32_t)
#define LC_PTRS_PRE_BLOCK                                                      \
    1024  // (DEFAULT_BLOCK_SIZE(4096) / LC_BLOCK_PTR_SIZE) = 1024
#define LC_FILE_NAME_MAX_LEN 256
#define LC_DIRECTORY_ENTRIES_PER_BLOCK 15 // 4096 / (256 + 4 + 4) = 15.5

// ---------- LCInode mode bits ----------
// File type high bits
#define LC_S_IFREG 0x8000  // Regular file
#define LC_S_IFDIR 0x4000  // Directory
#define LC_S_IFLNK 0xA000  // Symbolic link

// permissions low bits
#define LC_S_IRUSR 0x0100  // User read
#define LC_S_IWUSR 0x0080  // User write
#define LC_S_IXUSR 0x0040  // User execute
#define LC_S_IRGRP 0x0020  // Group read
#define LC_S_IROTH 0x0004  // Other read

#define LC_S_ISDIR(m) (((m) & 0xF000) == LC_S_IFDIR)
#define LC_S_ISREG(m) (((m) & 0xF000) == LC_S_IFREG)

// FUTURE: We can use inline data for small files stored in the block pointer
// array and use a flag to control it, but now we just use pointers
typedef struct LCInode {
    uint16_t mode;        // File type and permissions
    uint16_t uid;         // User ID of the owner
    uint64_t size;        // Size of the file in bytes
    uint32_t atime, ctime, mtime,
        dtime;            // Access, change, modification, and deletion times
    uint16_t gid;         // Group ID of the owner
    uint16_t link_count;  // Number of hard links to the file
    uint32_t blocks;
    uint32_t block_ptr[LC_BLOCK_PTRS_SIZE];  // Pointers to data blocks
    uint32_t generation;
    uint32_t cr_time;
    uint8_t  reserved[LC_INODE_SIZE -
                     LC_INODE_USED_SIZE];  // Reserved for future use
} __attribute__((packed)) LCInode;

typedef struct Stat {
    uint32_t ino;  // Inode number
    uint64_t size;
    uint32_t blocks;
    uint16_t mode, uid, gid;
    uint16_t links;
    uint32_t atime, mtime, ctime;
} __attribute__((packed)) Stat;

typedef struct LCDirEntry {
    uint32_t inode_id;
    uint32_t name_len;                    // Length of the name
    char     name[LC_FILE_NAME_MAX_LEN];  // Name of the
} __attribute__((packed)) LCDirEntry;

inline void *inode_clear(LCInode *inode) {
    memset(inode, 0, sizeof(LCInode));
    lc_memset(inode->block_ptr, LC_BLOCK_ILLEGAL_ID, sizeof(inode->block_ptr));
    return inode;
}

inline void *inode_as(LCInode *inode) {
    return reinterpret_cast<void *>(inode);
}

inline const void *inode_const_as(const LCInode *inode) {
    return reinterpret_cast<const void *>(inode);
}

#endif  // LC_INODE_H
