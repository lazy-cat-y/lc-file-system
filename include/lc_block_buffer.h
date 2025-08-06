#ifndef LC_BLOCK_BUFFER_H
#define LC_BLOCK_BUFFER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "lc_block.h"
#include "lc_block_device.h"
#include "lc_configs.h"
#include "lc_memory.h"
#include "lc_mpmc_queue.h"
#include "lc_thread_pool.h"
#include "lc_utils.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

typedef struct LCBlockBufferPoolFrame {
    bool     valid;        // Indicates if the frame is valid
    bool     dirty;        // Indicates if the frame has been modified
    uint8_t  ref_count;    // Reference count for the frame
    uint8_t  usage_count;  // Usage count for the frame, used for LRU
    uint32_t block_id;     // The ID of the block this frame holds
    LCBlock  block;        // The block data
} LCBlockBufferPoolFrame;

inline uint8_t add_lc_block_usage_count(uint8_t usage_count) {
    return std::min<uint8_t>(usage_count + 1, static_cast<uint8_t>(5));
}

// RAII
struct LCBlockFrameGuard {
    LCBlockBufferPoolFrame *frame;
    std::atomic_flag       *lock_flag;

    LCBlockFrameGuard(LCBlockBufferPoolFrame *f, std::atomic_flag *l) :
        frame(f),
        lock_flag(l) {}

    LCBlockFrameGuard(const LCBlockFrameGuard &)            = delete;
    LCBlockFrameGuard &operator=(const LCBlockFrameGuard &) = delete;

    LCBlockFrameGuard(LCBlockFrameGuard &&other) LC_NOEXCEPT
        : frame(other.frame),
          lock_flag(other.lock_flag) {
        other.lock_flag = nullptr;
        other.frame     = nullptr;
    }

    LCBlockFrameGuard &operator=(LCBlockFrameGuard &&other) = delete;

    // NOTE: no longer need to call unpin_block for access frame function
    ~LCBlockFrameGuard() {
        if (frame) {
            if (frame->ref_count > 0) {
                frame->ref_count--;
            }
        }
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

class LCBlockBufferPool {
public:

    LCBlockBufferPool() = delete;

    ~LCBlockBufferPool() {
        LC_ASSERT(!running_.load(), "Cannot destruct while running");
        free_resources();
    }

    LCBlockBufferPool(const LCBlockBufferPool &)            = delete;
    LCBlockBufferPool &operator=(const LCBlockBufferPool &) = delete;
    LCBlockBufferPool(LCBlockBufferPool &&)                 = delete;
    LCBlockBufferPool &operator=(LCBlockBufferPool &&)      = delete;

    LC_EXPLICIT LCBlockBufferPool(LCBlockDevice *block_device,
                                  uint32_t       pool_size,
                                  uint32_t       frame_interval_ms) :
        block_device_(block_device),
        pool_size_(pool_size),
        frame_interval_ms_(frame_interval_ms),
        clock_hand_(0) {
        init_resources();
    }

    void start() {
        bool expected = false;
        if (running_.compare_exchange_strong(expected, true)) {
            background_thread_ =
                std::thread(&LCBlockBufferPool::background_flush_loop, this);
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

    /*
     * read_block()
     * │─ lock mutex →
     * │   └─ check if block_id exists in frame_map_ →
     * │       ├─ if exists: get frame_index
     * │       └─ if not: find_or_allocate_frame(block_id) → insert into
     * frame_map_ ├─ unlock mutex   // can be packed with a new function └─ lock
     * frame_spinlock[frame_index] → │ ref_count++ → └─ return
     * frames_[frame_index].block.
     */
    LCBlock read_block(uint32_t block_id) {
        while (true) {
            uint32_t frame_index = find_frame(block_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];

                if (!frame.valid || frame.block_id != block_id) {
                    continue;
                }
                frame.ref_count++;
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                return frame.block;
            }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
        return LCBlock();  // Should never reach here, but return empty block
    }

    // This function copies the contents, ref_count is not incremented
    void read_block(uint32_t block_id, LCBlock &block) {
        while (true) {
            uint32_t frame_index = find_frame(block_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];

                if (!frame.valid || frame.block_id != block_id) {
                    continue;
                }
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                lc_memcpy(&block, &frame.block, DEFAULT_BLOCK_SIZE);
                return;
            }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
    }

    void read_block(uint32_t block_id, void *data, uint32_t size,
                    uint32_t offset = 0) {
        LC_ASSERT(size < DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");
        while (true) {
            uint32_t frame_index = find_frame(block_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];

                if (!frame.valid || frame.block_id != block_id) {
                    continue;
                }
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                lc_memcpy(static_cast<uint8_t *>(data),
                          &frame.block + offset,
                          size);
                return;
            }
        }
    }

    /*
     * write_block()
     * find_frame(block_id)
     * └─ lock frame_spinlock[frame_index] → write block data →
     * mark frame dirty → update frame_map_ with block_id and frame_index
     * └─ return
     */
    void write_block(uint32_t block_id, const LCBlock &block) {
        while (true) {
            uint32_t frame_index = find_frame(block_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];
                if (!frame.valid || frame.block_id != block_id) {
                    continue;  // Retry if the frame is not valid or does not
                               // match
                }
                lc_memcpy(&frame.block, &block, DEFAULT_BLOCK_SIZE);
                frame.dirty       = true;
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                return;
            }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
        // Should never reach here
    }

    void write_block(uint32_t block_id, const void *data, uint32_t size,
                     uint32_t offset = 0) {
        LC_ASSERT(size < DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");
        while (true) {
            uint32_t frame_index = find_frame(block_id);
            {
                FrameSpinLock lock(frame_locks_[frame_index]);
                auto         &frame = frames_[frame_index];
                if (!frame.valid || frame.block_id != block_id) {
                    continue;  // Retry if the frame is not valid or does not
                               // match
                }
                lc_memcpy(block_as(&frame.block) + offset, data, size);
                frame.dirty       = true;
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                return;
            }
        }
    }

    void unpin_block(uint32_t block_id) {
        std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);
        auto                                it = frame_map_.find(block_id);
        LC_ASSERT(it != frame_map_.end(), "Block ID not found");
        if (it != frame_map_.end()) {
            uint32_t      frame_index = it->second;
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];
            if (frame.valid && frame.block_id == block_id &&
                frame.ref_count > 0) {
                frame.ref_count--;
            }
        }
    }

    void flush_block(uint32_t block_id) {
        uint32_t frame_index = -1;
        {
            std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);
            auto                                it = frame_map_.find(block_id);
            if (it == frame_map_.end()) {
                return;
            }
            frame_index = it->second;
        }

        LC_ASSERT(frame_index != -1, "Frame index not found for block ID");

        {
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];

            if (!(frame.valid && frame.block_id == block_id)) {
                return;
            }

            if (frame.dirty) {
                block_device_->write_block(frame.block_id, frame.block);
                frame.dirty = false;
            }
        }
    }

    void flush_all() {
        for (uint32_t i = 0; i < pool_size_; ++i) {
            FrameSpinLock lock(frame_locks_[i]);
            auto         &frame = frames_[i];
            if (frame.valid && frame.dirty) {
                block_device_->write_block(frame.block_id, frame.block);
                frame.dirty = false;
            }
        }
    }

    LCBlockFrameGuard access_frame_lock(uint32_t block_id) {
        while (true) {
            uint32_t          frame_index = find_frame(block_id);
            std::atomic_flag &lock_flag   = frame_locks_[frame_index];

            while (lock_flag.test_and_set(std::memory_order_acquire)) {}

            LCBlockBufferPoolFrame &frame = frames_[frame_index];
            if (frame.valid && frame.block_id == block_id) {
                frame.ref_count++;
                frame.usage_count = add_lc_block_usage_count(frame.usage_count);
                return {&frame, &lock_flag};
            }

            lock_flag.clear(std::memory_order_release);
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

    /*
     * find_frame()
     * └─ lock mutex →
     *     └─ check if block_id exists in frame_map_ →
     *         ├─ if exists: get frame_index
     *         └─ if not: find_or_allocate_frame(block_id) → insert into
     * ├─ unlock mutex
     * └─ return frame_index
     */
    uint32_t find_frame(uint32_t block_id) {
        {
            std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);

            auto it = frame_map_.find(block_id);
            if (it != frame_map_.end()) {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> write(frame_map_mutex_);

        auto it = frame_map_.find(block_id);
        if (it != frame_map_.end()) {
            // If the block is already in the map, return its index
            return it->second;
        }

        uint32_t frame_index = evict_frame();
        {
            FrameSpinLock lock(frame_locks_[frame_index]);
            auto         &frame = frames_[frame_index];

            if (frame.valid && frame.block_id != block_id) {
                return find_frame(block_id);  // retry from top
            }

            if (frame.block_id != LC_BLOCK_ILLEGAL_ID) {
                frame_map_.erase(frame.block_id);
            }

            frame.block_id = block_id;
            frame.dirty    = false;
            block_device_->read_block(block_id, frame.block);
            frame.ref_count      = 0;
            frame.usage_count    = 1;
            frame_map_[block_id] = frame_index;
            frame.valid          = true;
        }
        LC_ASSERT(frame_index != -1, "Frame index not found for block ID");
        return frame_index;
    }

    // Clock sweep to find a reusable frame
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
                    // If the frame is dirty, flush it to disk
                    block_device_->write_block(frame.block_id, frame.block);
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

    // Background flush logic
    void background_flush_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            // condition variable will release the lock while waiting
            if (cv_.wait_for(lock,
                             std::chrono::milliseconds(frame_interval_ms_),
                             [this]() { return !running_; })) {
                break;
            }
            lock.unlock();
            flush_all();
            lock.lock();
        }
    }

    void init_resources() {
        frames_      = lc_alloc_array<LCBlockBufferPoolFrame>(pool_size_);
        frame_locks_ = lc_alloc_atomic_flag_array(pool_size_);
        for (uint32_t i = 0; i < pool_size_; ++i) {
            frames_[i].valid       = false;
            frames_[i].dirty       = false;
            frames_[i].ref_count   = 0;
            frames_[i].usage_count = 0;
            frames_[i].block_id    = LC_BLOCK_ILLEGAL_ID;
            block_clear(&frames_[i].block);
        }
    }

    void free_resources() {
        lc_free_array(frames_);
        frames_ = nullptr;
        lc_free_array(frame_locks_);
        frame_locks_ = nullptr;
    }

    // LCBlockManager         *block_manager_;
    LCBlockDevice          *block_device_;
    LCBlockBufferPoolFrame *frames_;
    uint32_t                pool_size_;
    uint32_t                frame_interval_ms_;
    std::unordered_map<uint32_t, uint32_t>
        frame_map_;   // Maps block_id to frame index

    std::atomic<uint32_t>
        clock_hand_;  // For clock algorithm, store the index of the next
                      // frame to check, this should be atomic

    // Concurrency
    std::mutex              mutex_;
    std::shared_mutex       frame_map_mutex_;  // Mutex for frame_map_
    std::condition_variable cv_;           // Condition variable for signaling
    std::thread             background_thread_;
    std::atomic<bool>       running_ {false};
    std::atomic_flag       *frame_locks_;  // Spin locks for each frame

    std::shared_ptr<LCThreadPool<LCWriteTaskPriority>> write_thread_pool_;
    std::shared_ptr<LCThreadPool<LCReadTaskPriority>>  read_thread_pool_;
    static constexpr const char *thread_name_ = "block_buffer_pool";
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_BUFFER_H
