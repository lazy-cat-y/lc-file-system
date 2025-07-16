#ifndef LC_INODE_BUFFER_H
#define LC_INODE_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_configs.h"
#include "lc_inode.h"
#include "lc_memory.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

typedef struct LCInodeBufferPoolFrame {
    bool     valid;
    bool     dirty;
    uint8_t  ref_count;
    uint8_t  usage_count;
    uint32_t inode_id;
    uint32_t block_id;  // Block ID where the inode is stored
    LCInode  inode;     // The inode data
} LCInodeBufferPoolFrame;

#define LC_INODE_ILLEGAL_ID 0xFFFFFFFF

inline uint8_t add_lc_inode_usage_count(uint8_t usage_count) {
    return std::min<uint8_t>(usage_count + 1, static_cast<uint8_t>(5));
}

class LCInodeBufferPool {
public:

    LCInodeBufferPool() = delete;

    LCInodeBufferPool(const LCInodeBufferPool &)            = delete;
    LCInodeBufferPool &operator=(const LCInodeBufferPool &) = delete;
    LCInodeBufferPool(LCInodeBufferPool &&)                 = delete;
    LCInodeBufferPool &operator=(LCInodeBufferPool &&)      = delete;

    ~LCInodeBufferPool() {
        LC_ASSERT(!running_.load(), "Cannot destruct while running");
        free_resources();
    }

    LC_EXPLICIT LCInodeBufferPool(LCBlockBufferPool *block_buffer_pool,
                                  uint32_t           pool_size,
                                  uint32_t           frame_interval_ms,
                                  uint32_t           inode_start_block_id,
                                  uint32_t           inode_block_count) :
        block_buffer_pool_(block_buffer_pool),
        pool_size_(pool_size),
        frame_interval_ms_(frame_interval_ms),
        inode_start_block_id_(inode_start_block_id),
        inode_block_count_(inode_block_count),
        clock_hand_(0) {
        init_frames();
    }

    void start();

    void stop();

    LCInode read_inode(uint32_t inode_id);

    void write_inode(uint32_t inode_id, const LCInode &inode);

    void unpin_inode(uint32_t inode_id);

    void flush_inode(uint32_t inode_id);

    void flush_all();

private:

    class FrameSpinLock {
    public:
        FrameSpinLock(std::atomic_flag &flag) : flag_(flag) {
            while (flag_.test_and_set(std::memory_order_acquire)) {}
        }

        ~FrameSpinLock() {
            flag_.clear(std::memory_order_release);
        }

        FrameSpinLock(const FrameSpinLock &)            = delete;
        FrameSpinLock &operator=(const FrameSpinLock &) = delete;
        FrameSpinLock(FrameSpinLock &&)                 = delete;
        FrameSpinLock &operator=(FrameSpinLock &&)      = delete;
    private:
        std::atomic_flag &flag_;
    };

    void init_frames() {
        frames_      = lc_alloc_array<LCInodeBufferPoolFrame>(pool_size_);
        frame_locks_ = lc_alloc_atomic_flag_array(pool_size_);
        for (uint32_t i = 0; i < pool_size_; ++i) {
            frames_[i].valid       = false;
            frames_[i].dirty       = false;
            frames_[i].ref_count   = 0;
            frames_[i].usage_count = 0;
            frames_[i].inode_id    = LC_INODE_ILLEGAL_ID;
            frames_[i].block_id    = LC_BLOCK_ILLEGAL_ID;
            inode_clear(&frames_[i].inode);
        }
    }

    void free_resources() {
        lc_free_array(frames_);
        frames_ = nullptr;
        lc_free_atomic_flag_array(frame_locks_);
        frame_locks_ = nullptr;
    }

    uint32_t find_frame(uint32_t inode_id) {
        {
            std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);

            auto it = frame_map_.find(inode_id);
            if (it != frame_map_.end()) {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> write(frame_map_mutex_);

        auto it = frame_map_.find(inode_id);
        if (it != frame_map_.end()) {
            return it->second;
        }

        uint32_t frame_index = evict_frame();
        {
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];

            if (frame.valid && frame.inode_id != inode_id) {
                return find_frame(inode_id);  // retry from top
            }

            if (frame.inode_id != LC_INODE_ILLEGAL_ID) {
                frame_map_.erase(frame.inode_id);
            }

            // READ INODE FROM BLOCK BUFFER POOL
            frame.inode_id = inode_id;
            frame.dirty    = false;
            frame.block_id = calc_block_id(inode_id);
            LCBlock block  = block_buffer_pool_->read_block(frame.block_id);
            memcpy(&frame.inode,
                   block_as_const(&block) + calc_offset(inode_id),
                   LC_INODE_SIZE);

            frame.ref_count      = 0;
            frame.usage_count    = 1;
            frame_map_[inode_id] = frame_index;
            frame.valid          = true;
        }
        return frame_index;
    }

    uint32_t evict_frame() {
        while (true) {
            uint32_t frame_index =
                clock_hand_.fetch_add(1, std::memory_order_relaxed) %
                pool_size_;
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];
            if (!frame.valid ||
                (frame.ref_count == 0 && frame.usage_count == 0)) {
                if (frame.valid && frame.dirty) {
                    // TODO: implement flush into block buffer pool
                    // read the block from the block buffer pool -> write inode
                    // into the block write the block back
                }

                frame.valid = false;
                frame.dirty = false;

                return frame_index;
            }
            if (frame.ref_count == 0 && frame.usage_count > 0) {
                frame.usage_count--;
            }
        }
        // Should never reach here
        LC_ASSERT(false, "No reusable frame found, this should not happen");
        return -1;
    }

    void background_flush_loop();

    uint32_t calc_block_id(uint32_t inode_id) const {
        LC_ASSERT(inode_id < inode_block_count_, "Inode ID out of range");
        return inode_start_block_id_ + inode_id / LC_INODES_PRE_BLOCK;
    }

    uint32_t calc_offset(uint32_t inode_id) const {
        LC_ASSERT(inode_id < inode_block_count_, "Inode ID out of range");
        return (inode_id % LC_INODES_PRE_BLOCK) * LC_INODE_SIZE;
    }

    // We can not hold the ownership of the block buffer pool here,
    // just point to it.
    LCBlockBufferPool                     *block_buffer_pool_;
    LCInodeBufferPoolFrame                *frames_;
    uint32_t                               pool_size_;
    uint32_t                               frame_interval_ms_;
    std::unordered_map<uint32_t, uint32_t> frame_map_;
    uint32_t                               inode_start_block_id_;
    uint32_t                               inode_block_count_;

    std::atomic<uint32_t> clock_hand_;  // For clock algorithm, store the index
                                        // of the next frame to check

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::shared_mutex       frame_map_mutex_;  // Mutex for frame_map_
    std::atomic<bool>       running_ {false};  // For background flush thread
    std::thread             background_thread_;
    std::atomic_flag       *frame_locks_;      // Spin locks for each frame
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_INODE_BUFFER_H
