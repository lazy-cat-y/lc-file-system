#ifndef LC_BLOCK_BUFFER_H
#define LC_BLOCK_BUFFER_H

#include <atomic>
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
#include "lc_task.h"
#include "lc_thread_pool.h"
#include "lc_trace_id_generator.h"
#include "lc_utils.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

/*
 * Invalid → ReadInProgress → ValidClean
 * ValidClean → Dirty → WriteInProgress → ValidClean
 * ValidClean → Evicting → Invalid
 */
enum class LCBlockBufferPoolFrameStatus {
    Invalid,
    ReadInProgress,
    ValidClean,
    Dirty,
    WriteInProgress,
    Evicting  // for eviction
};

// TODO change all original code to support atomic, the lock is not necessary
// for the frame, but it is necessary for the frame_map_ and frame_locks_
typedef struct LCBlockBufferPoolFrame {
    std::atomic<LCBlockBufferPoolFrameStatus>
                         status;     // Indicates if the frame has been modified
    std::atomic<uint8_t> ref_count;  // Reference count for the frame
    std::atomic<uint8_t>
        usage_count;                 // Usage count for the frame, used for LRU
    std::atomic<uint64_t> version;   // Status flags for the frame
    std::atomic<uint32_t> block_id;  // The ID of the block this frame holds
    LCBlock               block;     // The block data
} LCBlockBufferPoolFrame;

LC_NODISCARD LC_CONSTEXPR inline uint8_t __lc_add_lc_block_usage_count(
    uint8_t usage_count) {
    return lc_add_usage_count(usage_count, 5);
}

// RAII
struct LCBlockFrameGuard {
    LCBlockBufferPoolFrame *frame;
    std::atomic_flag       *lock_flag;

    LCBlockFrameGuard(LCBlockBufferPoolFrame *f, std::atomic_flag *l) :
        frame(f),
        lock_flag(l) {
        // TODO Atomic
        // frame->ref_count++;
    }

    LCBlockFrameGuard(const LCBlockFrameGuard &)            = delete;
    LCBlockFrameGuard &operator=(const LCBlockFrameGuard &) = delete;

    LCBlockFrameGuard(LCBlockFrameGuard &&other) :
        frame(other.frame),
        lock_flag(other.lock_flag) {
        other.frame     = nullptr;
        other.lock_flag = nullptr;
    }

    LCBlockFrameGuard &operator=(LCBlockFrameGuard &&other) {
        if (this != &other) {
            if (frame) {
                // TODO Atomic
                if (frame->ref_count > 0) {
                    frame->ref_count--;
                }
            }
            frame           = other.frame;
            lock_flag       = other.lock_flag;
            other.frame     = nullptr;
            other.lock_flag = nullptr;
        }
        return *this;
    }

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
            frame->status.store(LCBlockBufferPoolFrameStatus::Dirty,
                                std::memory_order_relaxed);
        }
    }
};

class LCBlockBufferPool {
    using FrameStatus = LCBlockBufferPoolFrameStatus;

    struct FrameIndexSlot {
        std::atomic<bool> ready;
        size_t            frame_index;
    };
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

    LC_EXPLICIT LCBlockBufferPool(
        LCBlockDevice *block_device, uint32_t pool_size,
        uint32_t                                           frame_interval_ms,
        std::shared_ptr<LCThreadPool<LCWriteTaskPriority>> write_thread_pool,
        std::shared_ptr<LCThreadPool<LCReadTaskPriority>>  read_thread_pool) :
        block_device_(block_device),
        pool_size_(pool_size),
        frame_interval_ms_(frame_interval_ms),
        clock_hand_(0),
        write_thread_pool_(std::move(write_thread_pool)),
        read_thread_pool_(std::move(read_thread_pool)) {
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
            flush_all(LCWriteTaskPriority::High);
        }
    }

    // This function copies the contents, ref_count is not incremented
    void read_block(uint32_t block_id, LCBlock &block) {
        while (true) {
            // uint32_t frame_index = find_frame(block_id);
            // {
            //     FrameSpinLock lock(frame_locks_[frame_index]);
            //     auto         &frame = frames_[frame_index];

            //     if (frame.status.load(std::memory_order_relaxed) ==
            //             LCBlockBufferPoolFrameStatus::Invalid ||
            //         frame.block_id != block_id) {
            //         continue;
            //     }
            //     frame.usage_count =
            //         __lc_add_lc_block_usage_count(frame.usage_count);
            //     lc_memcpy(&block, &frame.block, DEFAULT_BLOCK_SIZE);
            //     return;
            // }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
    }

    void read_block(uint32_t block_id, void *data, uint32_t size,
                    uint32_t offset = 0) {
        LC_ASSERT(size > 0, "Size must be positive");
        LC_ASSERT(offset >= 0, "Offset must be non-negative");
        LC_ASSERT(size < DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");

        while (true) {
            // uint32_t frame_index = find_frame(block_id);
            // {
            //     FrameSpinLock lock(frame_locks_[frame_index]);
            //     auto         &frame = frames_[frame_index];

            //     if (frame.status.load(std::memory_order_relaxed) ==
            //             LCBlockBufferPoolFrameStatus::Invalid ||
            //         frame.block_id != block_id) {
            //         continue;
            //     }
            //     frame.usage_count =
            //         __lc_add_lc_block_usage_count(frame.usage_count);
            //     lc_memcpy(static_cast<uint8_t *>(data),
            //               &frame.block + offset,
            //               size);
            //     return;
            // }
        }
    }

    void write_block(uint32_t block_id, const void *data, uint32_t size,
                     uint32_t offset = 0) {
        LC_ASSERT(size < DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");
        while (true) {
            // uint32_t frame_index = find_frame(block_id);
            // {
            //     FrameSpinLock lock(frame_locks_[frame_index]);
            //     auto         &frame = frames_[frame_index];
            //     if (!frame.valid || frame.block_id != block_id) {
            //         continue;  // Retry if the frame is not valid or does not
            //                    // match
            //     }
            //     lc_memcpy(block_as(&frame.block) + offset, data, size);
            //     frame.dirty = true;
            //     frame.usage_count =
            //         __lc_add_lc_block_usage_count(frame.usage_count);
            //     return;
            // }
        }
    }

    void unpin_block(uint32_t block_id) {
        // std::shared_lock<std::shared_mutex> shared_lock(frame_map_mutex_);
        // auto                                it = frame_map_.find(block_id);
        // LC_ASSERT(it != frame_map_.end(), "Block ID not found");
        // if (it != frame_map_.end()) {
        //     uint32_t      frame_index = it->second;
        //     FrameSpinLock lock(frame_locks_[frame_index]);
        //     auto         &frame = frames_[frame_index];
        //     if (frame.valid && frame.block_id == block_id &&
        //         frame.ref_count > 0) {
        //         frame.ref_count--;
        //     }
        // }
    }

    void flush_block(uint32_t block_id) {
        // uint32_t frame_index = -1;
        // {
        //     std::shared_lock<std::shared_mutex>
        //     shared_lock(frame_map_mutex_); auto it =
        //     frame_map_.find(block_id); if (it == frame_map_.end()) {
        //         return;
        //     }
        //     frame_index = it->second;
        // }

        // LC_ASSERT(frame_index != -1, "Frame index not found for block ID");

        // {
        //     FrameSpinLock lock(frame_locks_[frame_index]);
        //     auto         &frame = frames_[frame_index];

        //     if (!(frame.valid && frame.block_id == block_id)) {
        //         return;
        //     }

        //     if (frame.dirty) {
        //         block_device_->write_block(frame.block_id, frame.block);
        //         frame.dirty = false;
        //     }
        // }
    }

    void flush_all(LCWriteTaskPriority priority) {
        for (uint32_t i = 0; i < pool_size_; ++i) {
            LCBlockBufferPoolFrame &frame           = frames_[i];
            FrameStatus             expected_status = FrameStatus::Dirty;
            if (!frame.status.compare_exchange_strong(
                    expected_status,
                    FrameStatus::WriteInProgress,
                    std::memory_order_acq_rel)) {
                continue;  // Retry if the frame is not in the expected state
            }

            submit_flush_task(frame.block_id, i, priority, nullptr);
        }
    }

    void access_frame_lock(uint32_t block_id, LCBlockFrameGuard &guard) {
        // while (true) {
        //     uint32_t          frame_index = find_frame(block_id);
        //     std::atomic_flag &lock_flag   = frame_locks_[frame_index];

        //     while (lock_flag.test_and_set(std::memory_order_acquire)) {}

        //     LCBlockBufferPoolFrame &frame = frames_[frame_index];
        //     if (frame.valid && frame.block_id == block_id) {
        //         LCBlockFrameGuard guard(&frame, &lock_flag);
        //         frame.usage_count =
        //             __lc_add_lc_block_usage_count(frame.usage_count);
        //         return guard;
        //     }

        //     lock_flag.clear(std::memory_order_release);
        // }
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
    uint32_t find_frame(uint32_t block_id, LCReadTaskPriority priority) {
        while (true) {
            {
                std::shared_lock<std::shared_mutex> shared_lock(
                    frame_map_mutex_);

                auto it = frame_map_.find(block_id);
                if (it != frame_map_.end()) {
                    return it->second;  // Return existing frame index
                }
            }

            std::unique_lock<std::shared_mutex> write_lock(frame_map_mutex_);

            // Double-check locking to avoid ABA porblem, when we release the
            // the read lock, another thread may have inserted the block_id.
            auto it = frame_map_.find(block_id);
            if (it != frame_map_.end()) {
                return it->second;  // Return existing frame index
            }

            uint32_t    frame_index = evict_frame();
            auto       &frame       = frames_[frame_index];
            FrameStatus expected    = FrameStatus::Invalid;
            if (!frame.status.compare_exchange_strong(
                    expected,
                    FrameStatus::ReadInProgress,
                    std::memory_order_acq_rel)) {
                continue;  // Retry if the frame is not in the expected state
            }

            std::shared_ptr<std::atomic<bool>> cancel_token =
                std::make_shared<std::atomic<bool>>(false);
            submit_read_task(block_id, frame_index, priority, cancel_token);

            frame.block_id.store(block_id, std::memory_order_relaxed);
            frame.ref_count.store(0, std::memory_order_relaxed);
            frame.usage_count.store(1, std::memory_order_relaxed);
            frame_map_[block_id] = frame_index;  // Insert into frame_map_
            return frame_index;                  // Return the new frame index
        }
        LC_ASSERT(false, "No reusable frame found, this should not happen");
        return -1;                               // Should never reach here
    }

    // Clock sweep to find a reusable frame
    uint32_t evict_frame() {
        while (true) {
            uint32_t frame_index =
                clock_hand_.fetch_add(1, std::memory_order_relaxed) %
                pool_size_;
            auto &frame = frames_[frame_index];

            FrameStatus status = frame.status.load(std::memory_order_acquire);
            uint8_t ref_count = frame.ref_count.load(std::memory_order_acquire);
            uint8_t usage_count =
                frame.usage_count.load(std::memory_order_acquire);

            if (ref_count > 0) {
                continue;
            }

            if (usage_count > 0) {
                frame.usage_count.fetch_sub(1, std::memory_order_relaxed);
            }

            FrameStatus expected_status = FrameStatus::ValidClean;
            if (!frame.status.compare_exchange_strong(
                    expected_status,
                    FrameStatus::Evicting,
                    std::memory_order_acq_rel)) {
                continue;
            }

            uint64_t old_version =
                frame.version.load(std::memory_order_acquire);
            uint32_t old_block_id =
                frame.block_id.load(std::memory_order_relaxed);

            frame.status.store(FrameStatus::Invalid, std::memory_order_release);
            frame.block_id.store(LC_BLOCK_ILLEGAL_ID,
                                 std::memory_order_relaxed);
            frame.version.fetch_add(1, std::memory_order_relaxed);
            frame.usage_count.store(0, std::memory_order_relaxed);
            return frame_index;
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
            flush_all(LCWriteTaskPriority::Background);
            lock.lock();
        }
    }

    void submit_flush_task(uint32_t block_id, size_t frame_index,
                           LCWriteTaskPriority                priority,
                           std::shared_ptr<std::atomic<bool>> cancel_token) {
        LC_ASSERT(write_thread_pool_, "Write thread pool is not initialized");
        LCThreadPoolContextMetaData<LCWriteTaskPriority> meta;
        meta.listener_id = "block_buffer_pool";
        lc_generate_trace_id(LCTraceTypeID::WriteTask, meta.trace_id);
        meta.timestamp = std::time(nullptr);
        meta.priority  = priority;

        uint64_t frame_version =
            frames_[frame_index].version.load(std::memory_order_acquire);

        auto task = std::make_shared<LCLambdaTask<std::function<void()>>>(
            [this, block_id, frame_index, frame_version]() {
            // TODO add log for failure and success
            if (frames_[frame_index].status.load(std::memory_order_acquire) !=
                LCBlockBufferPoolFrameStatus::WriteInProgress) {
                return;  // Skip if the frame is not in the expected state
            }

            if (frames_[frame_index].version.load(std::memory_order_acquire) !=
                frame_version) {
                return;  // Skip if the frame version has changed
            }

            block_device_->write_block(block_id, frames_[frame_index].block);
            frames_[frame_index].status.store(
                LCBlockBufferPoolFrameStatus::ValidClean,
                std::memory_order_release);
        });
        LCTreadPoolContextFactory<LCWriteTaskPriority> context(meta,
                                                               task,
                                                               cancel_token);
        write_thread_pool_->wait_and_submit_task(context);
    }

    void submit_read_task(uint32_t block_id, size_t frame_index,
                          LCReadTaskPriority                 priority,
                          std::shared_ptr<std::atomic<bool>> cancel_token) {
        LC_ASSERT(read_thread_pool_, "Read thread pool is not initialized");
        LCThreadPoolContextMetaData<LCReadTaskPriority> meta;
        meta.listener_id = "block_buffer_pool";
        lc_generate_trace_id(LCTraceTypeID::ReadTask, meta.trace_id);
        meta.timestamp = std::time(nullptr);
        meta.priority  = priority;
        FrameIndexSlot slot {.frame_index = frame_index};
        slot.ready.store(false, std::memory_order_relaxed);

        uint64_t frame_version =
            frames_[frame_index].version.load(std::memory_order_acquire);

        auto task = std::make_shared<LCLambdaTask<std::function<void()>>>(
            [this, block_id, &slot, frame_version]() {
            if (frames_[slot.frame_index].status.load(
                    std::memory_order_acquire) !=
                LCBlockBufferPoolFrameStatus::ReadInProgress) {
                return;  // Skip if the frame is not in the expected state
            }
            if (frames_[slot.frame_index].version.load(
                    std::memory_order_acquire) != frame_version) {
                return;  // Skip if the frame version has changed
            }
            block_device_->read_block(block_id,
                                      frames_[slot.frame_index].block);
            frames_[slot.frame_index].status.store(
                LCBlockBufferPoolFrameStatus::ValidClean,
                std::memory_order_release);
            slot.ready.store(true, std::memory_order_release);
        });
        LCTreadPoolContextFactory<LCReadTaskPriority> context(meta,
                                                              task,
                                                              cancel_token);
        read_thread_pool_->wait_and_submit_task(context);

        while (!slot.ready.load(std::memory_order_acquire)) {
            // Spin until the task is ready
            std::this_thread::yield();
        }
    }

    void init_resources() {
        frames_      = lc_alloc_array<LCBlockBufferPoolFrame>(pool_size_);
        frame_locks_ = lc_alloc_atomic_flag_array(pool_size_);
        for (uint32_t i = 0; i < pool_size_; ++i) {
            frames_[i].status.store(LCBlockBufferPoolFrameStatus::Invalid,
                                    std::memory_order_relaxed);
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
