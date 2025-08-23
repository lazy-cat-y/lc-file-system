

#include <cassert>
#include <cstring>

#include "lc_block.h"
#include "lc_configs.h"
#include "lc_memory.h"

void block_clear(Block *block) {
    lc_memset(block->data, 0, DEFAULT_BLOCK_SIZE);
}

void block_write(Block *block, const void *data, size_t size, size_t offset) {
    LC_ASSERT(size <= DEFAULT_BLOCK_SIZE, "Size exceeds block size");
    LC_ASSERT(offset + size <= DEFAULT_BLOCK_SIZE,
              "Write exceeds block bounds");
    lc_memcpy(block->data + offset, data, size);
}

uint8_t *block_as(Block *block) {
    LC_ASSERT(block != nullptr, "Block pointer is null");
    return (uint8_t *)block->data;
}

const uint8_t *block_as_const(const Block *block) {
    LC_ASSERT(block != nullptr, "Block pointer is null");
    return (const uint8_t *)block->data;
}