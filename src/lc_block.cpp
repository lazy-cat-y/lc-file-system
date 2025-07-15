

#include <cassert>
#include <cstring>

#include "lc_block.h"
#include "lc_configs.h"

void block_clear(LCBlock *block) {
    memset(block->data, 0, DEFAULT_BLOCK_SIZE);
}

void block_write(LCBlock *block, const void *data, size_t size, size_t offset) {
    LC_ASSERT(size <= DEFAULT_BLOCK_SIZE, "Size exceeds block size");
    LC_ASSERT(offset + size <= DEFAULT_BLOCK_SIZE,
              "Write exceeds block bounds");
    memcpy(block->data + offset, data, size);
}

uint8_t *block_as(LCBlock *block) {
    LC_ASSERT(block != nullptr, "Block pointer is null");
    return (uint8_t *)block->data;
}

const uint8_t *block_as_const(const LCBlock *block) {
    LC_ASSERT(block != nullptr, "Block pointer is null");
    return (const uint8_t *)block->data;
}