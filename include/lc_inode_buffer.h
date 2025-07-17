#ifndef LC_INODE_BUFFER_H
#define LC_INODE_BUFFER_H

#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

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

inline void lc_write_inode_to_block_locked(LCInode *inode, LCBlock *block,
                                           uint32_t inode_id) {
    uint32_t offset = (inode_id % LC_INODES_PRE_BLOCK) * LC_INODE_SIZE;
    lc_memcpy(block_as(block) + offset, inode, LC_INODE_SIZE);
}

struct LCInodeFrameGuard {
    LCInodeBufferPoolFrame *frame;
    std::atomic_flag       *lock_flag;

    LCInodeFrameGuard(LCInodeBufferPoolFrame *f, std::atomic_flag *l) :
        frame(f),
        lock_flag(l) {}

    LCInodeFrameGuard(const LCInodeFrameGuard &)            = delete;
    LCInodeFrameGuard &operator=(const LCInodeFrameGuard &) = delete;

    LCInodeFrameGuard(LCInodeFrameGuard &&other) LC_NOEXCEPT
        : frame(other.frame),
          lock_flag(other.lock_flag) {
        other.lock_flag = nullptr;
        other.frame     = nullptr;
    }

    LCInodeFrameGuard &operator=(LCInodeFrameGuard &&other) = delete;

    ~LCInodeFrameGuard() {
        if (lock_flag) {
            lock_flag->clear(std::memory_order_release);
        }
    }

    void mark_dirty() {
        if (frame) {
            frame->dirty = true;
        }
    }
};

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
                                  uint32_t           inode_count) :
        block_buffer_pool_(block_buffer_pool),
        pool_size_(pool_size),
        frame_interval_ms_(frame_interval_ms),
        inode_start_block_id_(inode_start_block_id),
        inode_count_(inode_count),
        clock_hand_(0) {
        init_resources();
    }

    void start() {
        bool expected = false;
        if (running_.compare_exchange_strong(expected, true)) {
            background_thread_ =
                std::thread(&LCInodeBufferPool::background_flush_loop, this);
        }
    }

    void stop() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false)) {
            cv_.notify_all();
            if (background_thread_.joinable()) {
                background_thread_.join();
            }
            flush_all();
        }
    }

    LCInode read_inode(uint32_t inode_id) {
        while (true) {
            uint32_t frame_index = find_frame(inode_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];
                if (!frame.valid || frame.inode_id != inode_id) {
                    continue;
                }
                frame.ref_count++;
                frame.usage_count = add_lc_inode_usage_count(frame.usage_count);
                return frame.inode;
            }
        }
        LC_ASSERT(false, "Inode ID not found, this should not happen");
        return LCInode();  // Should never reach here, but return empty inode
    }

    void write_inode(uint32_t inode_id, const LCInode &inode) {
        while (true) {
            uint32_t frame_index = find_frame(inode_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];
                if (!frame.valid || frame.inode_id != inode_id) {
                    continue;
                }

                lc_memcpy(&frame.inode, &inode, LC_INODE_SIZE);
                frame.dirty = true;
                frame.ref_count++;
                frame.usage_count = add_lc_inode_usage_count(frame.usage_count);
                return;
            }
        }
        LC_ASSERT(false, "Inode ID not found, this should not happen");
    }

    LCInodeFrameGuard access_frame_lock(uint32_t inode_id) {
        while (true) {
            uint32_t          frame_index = find_frame(inode_id);
            std::atomic_flag &lock_flag   = frame_locks_[frame_index];
            while (lock_flag.test_and_set(std::memory_order_acquire)) {}

            auto &frame = frames_[frame_index];
            if (!frame.valid || frame.inode_id != inode_id) {
                lock_flag.clear(std::memory_order_release);
                continue;
            }
            frame.ref_count++;
            frame.usage_count = add_lc_inode_usage_count(frame.usage_count);
            return {&frame, &lock_flag};
        }
        LC_ASSERT(false, "Inode ID not found, this should not happen");
    }

    void unpin_inode(uint32_t inode_id) {
        std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);
        auto                                it = frame_map_.find(inode_id);
        LC_ASSERT(it != frame_map_.end(), "Inode ID not found");
        if (it != frame_map_.end()) {
            uint32_t      frame_index = it->second;
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];
            if (frame.valid && frame.inode_id == inode_id &&
                frame.ref_count > 0) {
                frame.ref_count--;
            }
        }
    }

    void flush_inode(uint32_t inode_id) {
        uint32_t frame_index = -1;
        {
            std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);
            auto                                it = frame_map_.find(inode_id);
            if (it != frame_map_.end()) {
                frame_index = it->second;
            }
        }

        LC_ASSERT(frame_index != -1, "Frame index not found for inode ID");
        bool     need_flush = false;
        uint32_t block_id   = 0;
        LCInode  inode_copy;

        {
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];

            if (frame.valid && frame.inode_id == inode_id && frame.dirty) {
                //  跨层 access_frame_lock() 和 unpin_block() 嵌套在 inode lock
                //  作用域内，可能导致交叉死锁。
                // auto blk_guard =
                //     block_buffer_pool_->access_frame_lock(frame.block_id);
                // lc_write_inode_to_block_locked(&frame.inode,
                //                                &blk_guard.frame->block,
                //                                frame.inode_id);
                // blk_guard.mark_dirty();
                // frame.dirty = false;
                // block_buffer_pool_->unpin_block(frame.block_id);
                block_id = frame.block_id;
                lc_memcpy(&inode_copy, &frame.inode, LC_INODE_SIZE);
                need_flush  = true;
                frame.dirty = false;
            }
        }

        if (need_flush) {
            {
                auto blk_guard =
                    block_buffer_pool_->access_frame_lock(block_id);
                lc_write_inode_to_block_locked(&inode_copy,
                                               &blk_guard.frame->block,
                                               inode_id);
                blk_guard.mark_dirty();
            }
            block_buffer_pool_->unpin_block(block_id);
        }
    }

    void flush_all() {
        // block_id, inode_id, LCInode
        std::vector<std::tuple<uint32_t, uint32_t, LCInode>> dirty_inodes;

        for (uint32_t i = 0; i < pool_size_; ++i) {
            bool     need_flush = false;
            uint32_t block_id   = 0;
            LCInode  inode_copy;
            uint32_t inode_id = 0;
            {
                FrameSpinLock lock(frame_locks_[i]);
                auto         &frame = frames_[i];
                if (frame.valid && frame.dirty) {
                    // auto blk_guard =
                    //     block_buffer_pool_->access_frame_lock(frame.block_id);
                    // lc_write_inode_to_block_locked(&frame.inode,
                    //                                &blk_guard.frame->block,
                    //                                frame.inode_id);
                    // blk_guard.mark_dirty();
                    // frame.dirty = false;
                    // block_buffer_pool_->unpin_block(frame.block_id);
                    block_id = frame.block_id;
                    inode_id = frame.inode_id;
                    lc_memcpy(&inode_copy, &frame.inode, LC_INODE_SIZE);
                    need_flush  = true;
                    frame.dirty = false;
                }

                if (need_flush) {
                    dirty_inodes.emplace_back(block_id,
                                              inode_id,
                                              std::move(inode_copy));
                }
            }
        }

        for (auto &[block_id, inode_id, inode] : dirty_inodes) {
            {
                auto blk_guard =
                    block_buffer_pool_->access_frame_lock(block_id);
                lc_write_inode_to_block_locked(&inode,
                                               &blk_guard.frame->block,
                                               inode_id);
                blk_guard.mark_dirty();
            }
            block_buffer_pool_->unpin_block(block_id);
        }
    }

private:

    class FrameSpinLock {
    public:
        LC_EXPLICIT FrameSpinLock(std::atomic_flag &flag) : flag_(flag) {
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

    void init_resources() {
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
            lc_memcpy(&frame.inode,
                      block_as_const(&block) + calc_offset(inode_id),
                      LC_INODE_SIZE);
            block_buffer_pool_->unpin_block(frame.block_id);

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
                    {
                        auto blk_guard = block_buffer_pool_->access_frame_lock(
                            frame.block_id);

                        lc_write_inode_to_block_locked(&frame.inode,
                                                       &blk_guard.frame->block,
                                                       frame.inode_id);
                        blk_guard.mark_dirty();
                        frame.dirty = false;
                    }
                    block_buffer_pool_->unpin_block(frame.block_id);
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

    void background_flush_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            if (cv_.wait_for(lock,
                             std::chrono::milliseconds(frame_interval_ms_),
                             [this] { return !running_; })) {
                break;
            }
            lock.unlock();
            flush_all();
            lock.lock();
        }
    }

    uint32_t calc_block_id(uint32_t inode_id) const {
        LC_ASSERT(inode_id < inode_count_, "Inode ID out of range");
        return inode_start_block_id_ + inode_id / LC_INODES_PRE_BLOCK;
    }

    uint32_t calc_offset(uint32_t inode_id) const {
        LC_ASSERT(inode_id < inode_count_, "Inode ID out of range");
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
    uint32_t                               inode_count_;

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
