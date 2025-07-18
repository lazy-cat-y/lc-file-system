#ifndef LC_BLOCK_MANAGER_H
#define LC_BLOCK_MANAGER_H

#include <sys/stat.h>

#include <cstdint>

#include "lc_bitmap.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

class LCBlockManager {
public:
    LCBlockManager() = delete;

    ~LCBlockManager() {
        block_buffer_pool_ = nullptr;
        super_block_       = nullptr;
    }

    LCBlockManager(const LCBlockManager &)            = delete;
    LCBlockManager &operator=(const LCBlockManager &) = delete;
    LCBlockManager(LCBlockManager &&)                 = delete;
    LCBlockManager &operator=(LCBlockManager &&)      = delete;

    LC_EXPLICIT LCBlockManager(LCBlockBufferPool *block_buffer_pool,
                               LCSuperBlock      *super_block) :
        block_buffer_pool_(block_buffer_pool),
        super_block_(super_block) {
        LC_ASSERT(block_buffer_pool_ != nullptr,
                  "Block buffer pool cannot be null");
        LC_ASSERT(super_block_ != nullptr, "Super block cannot be null");
    }

    void read_block(uint32_t block_id, LCBlock &block) const {
        LC_ASSERT(block_id < super_block_->total_blocks, "Invalid block ID");

#if defined(DEBUG)
        {
            // The block must be allocated before reading
            LCBitmapIndex index =
                lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
            LC_ASSERT(index.block_id < super_block_->total_blocks,
                      "Block ID out of bounds");
            LCBlockFrameGuard bitmap_frame_guard =
                block_buffer_pool_->access_frame_lock(index.block_id);
            LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
                          (1 << index.bit_offset),
                      "Block is not allocated");
        }
#endif  // DEBUG

        block_buffer_pool_->read_block(block_id, block);
    }

    void write_block(uint32_t block_id, const LCBlock &block) {
        LC_ASSERT(block_id < super_block_->total_blocks, "Invalid block ID");

#if defined(DEBUG)
        {
            // The block must be allocated before writing
            LCBitmapIndex index =
                lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
            LC_ASSERT(index.block_id < super_block_->total_blocks,
                      "Block ID out of bounds");
            LCBlockFrameGuard bitmap_frame_guard =
                block_buffer_pool_->access_frame_lock(index.block_id);
            LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
                          (1 << index.bit_offset),
                      "Block is not allocated");
        }
#endif  // DEBUG

        block_buffer_pool_->write_block(block_id, block);
    }

    // TODO: Implement block free and alloc, and we need to make sure
    // TODO: init inode block the block ptr should set up to illegal ID
    // that the block is allocated before writing or reading.
    uint32_t alloc_block() {
        // Find a free block in the bitmap
        for (uint32_t bitmap_block_index = 0; bitmap_block_index < super_block_->total_blocks;
             ++bitmap_block_index) {
            uint32_t bitmap_block_id = super_block_->block_bitmap_start + bitmap_block_index;
            
            LCBlockFrameGuard bitmap_frame_guard =
                block_buffer_pool_->access_frame_lock(bitmap_block_id);
            LCBlock &bitmap_block = bitmap_frame_guard.frame->block;

        }
    }

    // TODO: set block bitmap
    void free_block(uint32_t block_id) {}

private:

    LCBlockBufferPool *block_buffer_pool_;
    LCSuperBlock      *super_block_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_MANAGER_H
