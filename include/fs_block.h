#ifndef FS_BLOCK_H
#define FS_BLOCK_H

#include "fs_config.h"
#include "fs_memory.h"
#include "fs_types.h"
#include "fs_utils.h"

FS_NAMESPACE_BEGIN

struct Block {
    alignas(DEFAULT_BLOCK_SIZE) uint8 data[DEFAULT_BLOCK_SIZE];
};

inline void block_clear(Block *block) {
    fs_memset(block->data, 0, DEFAULT_BLOCK_SIZE);
}

inline void block_write(Block *block, const void *data, size_t size,
                        size_t offset) {
    ASSERT(size <= DEFAULT_BLOCK_SIZE, "Size exceeds block size");
    ASSERT(offset + size <= DEFAULT_BLOCK_SIZE, "Write exceeds block bounds");
    fs_memcpy(block->data + offset, data, size);
}

inline uint8 *block_as(Block *block) {
    ASSERT(block != nullptr, "Block pointer is null");
    return (uint8 *)block->data;
}

enum class IncompatType : uint32 {
    INCOMPAT_HAS_JOURNAL      = 1u << 0,
    INCOMPAT_EXTERNAL_JOURNAL = 1u << 1,
    INCOMPAT_64BIT            = 1u << 2
};

enum class JournalType : uint8 {
    None      = 0,
    Inode     = 1,
    FixedArea = 2,
    External  = 3,
};

struct SuperBlock {
    le32 inode_count;
    le64 block_count;

    le32 free_inode_count;
    le64 free_block_count;

    le64 first_data_block;

    le32 log_block_size;

    le32 block_pre_group;
    le32 inode_pre_group;

    le16 log_inode_size;
    le16 inode_size_reserved;

    le32 magic;

    u8 uuid[16];

    le64 mtime;
    le64 wtime;

    le32 incompat_flags;
    u8   jnl_type;
    u8   jnl_csum_type;
    le16 jnl_reserved;

    union {
        struct {
            le64 jnl_inode;
        } in_inum;

        struct {
            le64 jnl_start_block;
            le64 jnl_len_blocks;
        } in_fixed;

        struct {
            u8 jnl_uuid[16];
        } external;
    } jnl;

    u8   csum_type;
    u8   csum_reserved[3];
    le32 csum;
} __attribute__((packed));

FS_NAMESPACE_END

#endif  // FS_BLOCK_H
