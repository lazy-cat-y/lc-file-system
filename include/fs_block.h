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

struct SuperBlock {
    uint32 magic      = BLOCK_MAGIC_NUMBER;
    uint32 block_size = DEFAULT_BLOCK_SIZE;
    uint8  version    = 2;

    uint32 img_total_blocks;

    // —— Reserve: WAL Layout (only describes position and size, not used yet)
    // WAL_SZ = 1/32 MAX: 128MB
    uint32 l_start;                      // WAL Start block
    uint32 l_total_blocks;               // WAL Total blocks
    uint32 l_seg_blocks = L_SEG_BLOCKS;  // WAL Segment size in blocks

    uint32 block_bitmap_start;           // = wal_start + wal_total_blocks

    uint32 inode_count;
    uint32 inode_bitmap_start;  // = block_bitmap_start + number of blocks of
                                // block_bitmap

    uint32 inode_block_count;
    uint32 inode_block_start;   // = inode_bitmap_start + number of blocks of
                                // inode_bitmap

    uint32 data_start;          // = inode_start + inode_block_count

    uint32 crc;
} __attribute__((packed));

FS_NAMESPACE_END

#endif  // FS_BLOCK_H
