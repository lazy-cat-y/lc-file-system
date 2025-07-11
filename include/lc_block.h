#ifndef LC_BLOCK_H
#define LC_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <cstdint>

#include "lc_configs.h"

LC_EXTERN_C_BEGIN

#define DEFAULT_BLOCK_SIZE 4096
#define BLOCK_MAGIC_NUMBER 0xDEADBEEF

typedef struct LCBlock {
    uint8_t data[DEFAULT_BLOCK_SIZE];  // Pointer to the block data
} LCBlock;

void block_clear(LCBlock *block);

void block_write(LCBlock *block, const void *data, size_t size,
                 size_t offset = 0);

void *block_as(LCBlock *block);

const void *block_as_const(const LCBlock *block);


struct LCBlockHeader {
    uint32_t magic      = BLOCK_MAGIC_NUMBER;
    uint32_t block_size = DEFAULT_BLOCK_SIZE;
    uint32_t total_blocks;
    uint32_t block_bitmap_start;  // usually = 1

    uint32_t inode_count;
    uint32_t inode_bitmap_start;
    uint32_t inode_start;   // = bitmap_start + bitmap_blocks
    uint32_t inode_block_count;
    uint32_t data_start;    // = inode_start + inode_block_count
} __attribute__((packed));

LC_EXTERN_C_END

#endif  // LC_BLOCK_H
