#ifndef LC_BLOCK_MANAGER_H
#define LC_BLOCK_MANAGER_H

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>

#include "lc_bitmap.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_configs.h"
#include "lc_utils.h"

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

    LC_EXPLICIT LCBlockManager(LCBlockBufferPool  *block_buffer_pool,
                               const LCSuperBlock *super_block) :
        block_buffer_pool_(block_buffer_pool),
        super_block_(super_block) {
        LC_ASSERT(block_buffer_pool_ != nullptr,
                  "Block buffer pool cannot be null");
        LC_ASSERT(super_block_ != nullptr, "Super block cannot be null");
        block_bitmap_size_ =
            super_block_->inode_bitmap_start - super_block_->block_bitmap_start;
        data_start_index_ =
            lc_cal_bitmap_index(super_block_->block_bitmap_start,
                                super_block->data_start);
    }

    void read_block(uint32_t block_id, LCBlock &block) {
        LC_ASSERT(block_id < super_block_->total_blocks, "Invalid block ID");

#if defined(DEBUG)
        // {
        //     // The block must be allocated before reading
        //     LCBitmapIndex index =
        //         lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
        //     LC_ASSERT(index.block_id < super_block_->total_blocks,
        //               "Block ID out of bounds");
        //     LCBlockFrameGuard bitmap_frame_guard =
        //         block_buffer_pool_->access_frame_lock(index.block_id);
        //     LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
        //     LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
        //                   (1 << index.bit_offset),
        //               "Block is not allocated");
        // }
#endif  // DEBUG

        block_buffer_pool_->read_block(block_id, block);
    }

    void read_block(uint32_t block_id, void *data, uint32_t size,
                    uint32_t offset = 0) {
        LC_ASSERT(block_id < super_block_->total_blocks, "Invalid block ID");
#if defined(DEBUG)
        {
            LCBitmapIndex index =
                lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
            LC_ASSERT(index.block_id < super_block_->total_blocks,
                      "Block ID out of bounds");
            // LCBlockFrameGuard bitmap_frame_guard =
            //     block_buffer_pool_->access_frame_lock(index.block_id);
            // LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            // LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
            //               (1 << index.bit_offset),
            //           "Block is not allocated");
        }
#endif  // DEBUG
        block_buffer_pool_->read_block(block_id, data, size, offset);
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
            // LCBlockFrameGuard bitmap_frame_guard =
            //     block_buffer_pool_->access_frame_lock(index.block_id);
            // LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            // LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
            //               (1 << index.bit_offset),
            //           "Block is not allocated");
        }
#endif  // DEBUG

        // block_buffer_pool_->write_block(block_id, block);
    }

    void write_block(uint32_t block_id, const void *data, uint32_t size,
                     uint32_t offset = 0) {
        LC_ASSERT(block_id < super_block_->total_blocks, "Invalid block ID");
#if defined(DEBUG)
        {
            // The block must be allocated before writing
            LCBitmapIndex index =
                lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
            LC_ASSERT(index.block_id < super_block_->total_blocks,
                      "Block ID out of bounds");
            // LCBlockFrameGuard bitmap_frame_guard =
            //     block_buffer_pool_->access_frame_lock(index.block_id);
            // LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            // LC_ASSERT(block_as_const(&bitmap_block)[index.byte_offset] &
            //               (1 << index.bit_offset),
            //           "Block is not allocated");
        }
#endif  // DEBUG

        block_buffer_pool_->write_block(block_id, data, size, offset);
    }

    // that the block is allocated before writing or reading.
    uint32_t alloc_block() {
        // Find a free block in the bitmap
        for (uint32_t bitmap_block_index = data_start_index_.block_id;
             bitmap_block_index < block_bitmap_size_;
             ++bitmap_block_index) {
            // {
            //     LCBlockFrameGuard bitmap_frame_guard =
            //         block_buffer_pool_->access_frame_lock(bitmap_block_index);
            //     LCBlock &bitmap_block = bitmap_frame_guard.frame->block;
            //     uint8_t *bitmap_data  = block_as(&bitmap_block);
            //     uint32_t start_byte =
            //         (bitmap_block_index == data_start_index_.block_id)
            //             ? data_start_index_.byte_offset
            //             : 0;
            //     for (uint32_t byte_offset = start_byte;
            //          byte_offset < DEFAULT_BLOCK_SIZE;
            //          ++byte_offset) {
            //         if (bitmap_data[byte_offset] == 0xFF) {
            //             continue;
            //         }
            //         uint32_t start_bit =
            //             (bitmap_block_index == data_start_index_.block_id &&
            //              byte_offset == data_start_index_.byte_offset)
            //                 ? data_start_index_.bit_offset
            //                 : 0;
            //         for (uint32_t bit_offset = (start_bit); bit_offset < 8;
            //              ++bit_offset) {
            //             if (!(bitmap_data[byte_offset] & (1 << bit_offset))) {
            //                 bitmap_data[byte_offset] |= (1 << bit_offset);
            //                 uint32_t block_id =
            //                     (bitmap_block_index -
            //                      super_block_->block_bitmap_start) *
            //                         LC_BITS_PER_BLOCK +
            //                     byte_offset * 8 + bit_offset;
            //                 LC_ASSERT(block_id < super_block_->total_blocks,
            //                           "Block ID out of bounds");
            //                 bitmap_frame_guard.mark_dirty();
            //                 return block_id;
            //             }
            //         }
            //     }
            // }
        }
        // No free block found, TODO: handle this case in the higher
        // level
        return LC_BLOCK_ILLEGAL_ID;
    }

    void free_block(uint32_t block_id) {
        LC_ASSERT(block_id < super_block_->total_blocks,
                  "Block ID out of bounds");
        LCBitmapIndex index =
            lc_cal_bitmap_index(super_block_->block_bitmap_start, block_id);
        LC_ASSERT(index.block_id < super_block_->inode_bitmap_start,
                  "Block Bitmap ID out of bounds");

        // {
        //     LCBlockFrameGuard data_guard =
        //         block_buffer_pool_->access_frame_lock(block_id);

        //     block_clear(&data_guard.frame->block);
        //     data_guard.mark_dirty();
        // }
        // {
        //     LCBlockFrameGuard bmp_guard =
        //         block_buffer_pool_->access_frame_lock(index.block_id);

        //     uint8_t *bmp = block_as(&bmp_guard.frame->block);
        //     LC_ASSERT(bmp[index.byte_offset] & (1 << index.bit_offset),
        //               "Block is not allocated");
        //     bmp[index.byte_offset] &= ~(1 << index.bit_offset);
        //     bmp_guard.mark_dirty();
        // }
    }

private:

    LCBlockBufferPool  *block_buffer_pool_;
    const LCSuperBlock *super_block_;
    uint32_t            block_bitmap_size_;
    LCBitmapIndex       data_start_index_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_MANAGER_H
