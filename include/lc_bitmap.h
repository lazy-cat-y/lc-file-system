#ifndef LC_BITMAP_H
#define LC_BITMAP_H

#include <cstdint>
#include <memory>

#include "lc_configs.h"
#include "lc_utils.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

typedef struct BitmapIndex {
    uint32_t block_id;     // The ID of the block in the bitmap
    uint32_t byte_offset;  // The offset in the bitmap (in bytes)
    uint8_t  bit_offset;   // The bit offset in the block
} BitmapIndex;

inline BitmapIndex lc_cal_bitmap_index(uint32_t block_offset, uint32_t id) {
    BitmapIndex index;
    index.block_id            = block_offset + id / LC_BITS_PER_BLOCK;
    uint32_t block_bit_offset = id % LC_BITS_PER_BLOCK;
    index.byte_offset         = block_bit_offset / 8;
    index.bit_offset          = block_bit_offset % 8;
    return index;
}

class LCBitmapCache {
    // TODO: Implement a bitmap cache for block manager and inode manager
    // FUTURE:

private:
    std::unique_ptr<uint8_t[]> bitmap_;
    uint32_t                   start_block_id_;
    uint32_t                   block_count_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BITMAP_H
