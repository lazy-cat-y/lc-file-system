
#ifndef LC_INODE_H
#define LC_INODE_H

#include <cstdint>

#define LC_INODE_SIZE       256
#define LC_INODE_USED_SIZE  104
#define LC_DIRECT_PTRS      12
#define LC_INODES_PRE_BLOCK 16  // 4096 / 256

typedef struct __attribute__((packed)) LCInode {
    uint16_t mode;              // File type and permissions
    uint16_t uid;               // User ID of the owner
    uint64_t size;              // Size of the file in bytes
    uint32_t atime, ctime, mtime,
        dtime;            // Access, change, modification, and deletion times
    uint16_t gid;         // Group ID of the owner
    uint16_t link_count;  // Number of hard links to the file
    uint32_t blocks;
    uint32_t block_ptr[LC_DIRECT_PTRS + 3];  // Pointers to data blocks
    uint32_t generation;
    uint32_t cr_time;
    uint8_t  reserved[LC_INODE_SIZE -
                     LC_INODE_USED_SIZE];  // Reserved for future use
} LCInode;

struct Stat {
    uint64_t size;
    uint32_t blocks;
    uint16_t mode, uid, gid;
    uint16_t links;
    uint32_t atime, mtime, ctime;
};

#endif  // LC_INODE_H
