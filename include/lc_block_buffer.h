#ifndef LC_BLOCK_BUFFER_H
#define LC_BLOCK_BUFFER_H

#include <sys/types.h>

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstddef>
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
#include "lc_wait_strategy.h"

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

enum class LCBlockFrameGuardLockType {
    Read,
    Write,
};

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

LC_CONSTEXPR inline void __lc_add_lc_block_usage_count(
    std::atomic<uint8_t> &usage_count) {
    increment_usage_count_if_not_max(usage_count, 5);
}

struct LCBlockFrameGuard {
    using Frame              = LCBlockBufferPoolFrame;
    using ReadLock           = std::shared_lock<std::shared_mutex>;
    using WriteLock          = std::unique_lock<std::shared_mutex>;
    using FrameGuardLockType = LCBlockFrameGuardLockType;

    std::shared_ptr<std::shared_mutex> lock;   // Shared mutex for the frame
    ReadLock                           read_lock;
    WriteLock                          write_lock;
    std::shared_ptr<Frame>             frame;  // Pointer to the frame
    uint64_t                           version;
    FrameGuardLockType                 lock_type;

    LCBlockFrameGuard(Frame *frame, uint64_t version,
                      std::shared_ptr<std::shared_mutex> lock,
                      FrameGuardLockType                 lock_type) :
        frame(std::shared_ptr<Frame>(frame, [](Frame *) {})),
        version(version),
        lock(std::move(lock)),
        read_lock(*lock, std::defer_lock),
        write_lock(*lock, std::defer_lock),
        lock_type(lock_type) {
        if (lock_type == FrameGuardLockType::Read) {
            read_lock.lock();
        } else if (lock_type == FrameGuardLockType::Write) {
            write_lock.lock();
        } else {
            LC_ASSERT(false, "Invalid lock type for LCBlockFrameGuard");
        }
        if (frame) {
            frame->ref_count.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    LCBlockFrameGuard() = default;

    LCBlockFrameGuard(const LCBlockFrameGuard &)            = delete;
    LCBlockFrameGuard &operator=(const LCBlockFrameGuard &) = delete;

    LCBlockFrameGuard(LCBlockFrameGuard &&other) :
        frame(std::move(other.frame)),
        lock(std::move(other.lock)),
        read_lock(std::move(other.read_lock)),
        write_lock(std::move(other.write_lock)),
        version(other.version),
        lock_type(other.lock_type) {
        other.frame = nullptr;  // Prevent double decrement
        other.lock  = nullptr;  // Prevent double decrement
    }

    LCBlockFrameGuard &operator=(LCBlockFrameGuard &&other) {
        if (this != &other) {
            if (frame) {
                frame->ref_count.fetch_sub(1, std::memory_order_acq_rel);
            }
            frame       = std::move(other.frame);
            lock        = std::move(other.lock);
            read_lock   = std::move(other.read_lock);
            write_lock  = std::move(other.write_lock);
            version     = other.version;
            lock_type   = other.lock_type;
            other.frame = nullptr;  // Prevent double decrement
            other.lock  = nullptr;  // Prevent double decrement
        }
        return *this;
    }

    // NOTE: no longer need to call unpin_block for access frame function
    ~LCBlockFrameGuard() {
        if (frame) {
            frame->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool is_valid() const {
        return frame &&
               frame->version.load(std::memory_order_acquire) == version &&
               frame->status.load(std::memory_order_acquire) !=
                   LCBlockBufferPoolFrameStatus::Invalid;
    }

    void mark_dirty() {
        if (frame) {
            frame->status.store(LCBlockBufferPoolFrameStatus::Dirty,
                                std::memory_order_release);
        }
    }
};

class LCBlockBufferPool {
    using Frame         = LCBlockBufferPoolFrame;
    using FrameStatus   = LCBlockBufferPoolFrameStatus;
    using FrameGuard    = LCBlockFrameGuard;
    using FrameLockType = LCBlockFrameGuardLockType;

    struct FrameIndexSlot {
        std::atomic<bool> ready;
        size_t            frame_index;
    };
public:

    LCBlockBufferPool() = delete;

    ~LCBlockBufferPool() {
        // LC_ASSERT(!running_.load(), "Cannot destruct while running");
        free_resources();
    }

    LCBlockBufferPool(const LCBlockBufferPool &)            = delete;
    LCBlockBufferPool &operator=(const LCBlockBufferPool &) = delete;
    LCBlockBufferPool(LCBlockBufferPool &&)                 = delete;
    LCBlockBufferPool &operator=(LCBlockBufferPool &&)      = delete;

    LC_EXPLICIT LCBlockBufferPool(
        LCBlockDevice *block_device, uint32_t pool_size,
        uint32_t                                      frame_interval_ms,
        std::shared_ptr<LCThreadPool<LCTaskPriority>> write_thread_pool,
        std::shared_ptr<LCThreadPool<LCTaskPriority>> read_thread_pool);

    //     block_device_(block_device),
    //     pool_size_(pool_size),
    //     frame_interval_ms_(frame_interval_ms),
    //     clock_hand_(0),
    //     write_thread_pool_(std::move(write_thread_pool)),
    //     read_thread_pool_(std::move(read_thread_pool)) {
    //     init_resources();
    // }

    void start() {
        // bool expected = false;
        // if (running_.compare_exchange_strong(expected, true)) {
        //     background_thread_ =
        //         std::thread(&LCBlockBufferPool::background_flush_loop, this);
        // }
    }

    void stop() {
        // bool expected = true;
        // if (running_.compare_exchange_strong(expected, false)) {
        //     wait_strategy_->notify_all();
        //     if (background_thread_.joinable()) {
        //         background_thread_.join();
        //     }
        //     flush_all(LCTaskPriority::High);
        // }
    }

    // This function copies the contents, ref_count is not incremented
    void read_block(uint32_t block_id, LCBlock &block,
                    LCTaskPriority priority) {
        // while (true) {
        //     uint32_t frame_index = find_frame(block_id, priority);
        //     {
        //         FrameReadWriteLock lock(frame_locks_[frame_index],
        //                                 FrameLockType::Read);

        //         Frame &frame  = frames_[frame_index];
        //         auto   status = frame.status.load(std::memory_order_relaxed);
        //         if ((status != FrameStatus::ValidClean &&
        //              status != FrameStatus::Dirty) ||
        //             frame.block_id.load(std::memory_order_relaxed) !=
        //                 block_id) {
        //             continue;  // Retry if the frame is not valid or does not
        //                        // match
        //         }
        //         __lc_add_lc_block_usage_count(frame.usage_count);
        //         lc_memcpy(&block, &frame.block, DEFAULT_BLOCK_SIZE);
        //         return;
        //     }
        // }
        // LC_ASSERT(false, "Block ID not found, this should not happen");
    }

    void read_block(uint32_t block_id, LCTaskPriority priority, void *data,
                    uint32_t size, uint32_t offset = 0) {
        // LC_ASSERT(size > 0, "Size must be positive");
        // LC_ASSERT(offset >= 0, "Offset must be non-negative");
        // LC_ASSERT(size <= DEFAULT_BLOCK_SIZE - offset,
        //           "Size exceeds block size minus offset");

        // while (true) {
        //     LC_ASSERT(data != nullptr, "Data pointer cannot be null");
        //     uint frame_index = find_frame(block_id, priority);
        //     {
        //         FrameReadWriteLock lock(frame_locks_[frame_index],
        //                                 FrameLockType::Read);
        //         Frame             &frame = frames_[frame_index];
        //         auto status = frame.status.load(std::memory_order_relaxed);
        //         if ((status != FrameStatus::ValidClean &&
        //              status != FrameStatus::Dirty) ||
        //             frame.block_id.load(std::memory_order_relaxed) !=
        //                 block_id) {
        //             continue;  // Retry if the frame is not valid or does not
        //                        // match
        //         }
        //         __lc_add_lc_block_usage_count(frame.usage_count);
        //         lc_memcpy(static_cast<uint8_t *>(data),
        //                   block_as(&frame.block) + offset,
        //                   size);
        //         return;
        //     }
        // }
    }

    void write_block(uint32_t block_id, LCTaskPriority priority,
                     const void *data, uint32_t size, uint32_t offset = 0) {
        // LC_ASSERT(size <= DEFAULT_BLOCK_SIZE - offset,
        //           "Size exceeds block size minus offset");
        // while (true) {
        //     LC_ASSERT(data != nullptr, "Data pointer cannot be null");
        //     uint32_t frame_index = find_frame(block_id, priority);
        //     {
        //         FrameReadWriteLock lock(frame_locks_[frame_index],
        //                                 FrameLockType::Write);
        //         Frame             &frame = frames_[frame_index];

        //         if (frame.status.load(std::memory_order_relaxed) ==
        //                 LCBlockBufferPoolFrameStatus::Invalid ||
        //             frame.block_id.load(std::memory_order_relaxed) !=
        //                 block_id) {
        //             continue;  // Retry if the frame is not valid or does not
        //                        // match
        //         }

        //         lc_memcpy(block_as(&frame.block) + offset, data, size);
        //         frame.status.store(LCBlockBufferPoolFrameStatus::Dirty,
        //                            std::memory_order_release);
        //         __lc_add_lc_block_usage_count(frame.usage_count);
        //         return;
        //     }
        // }
    }

    void flush_block(uint32_t block_id, LCTaskPriority priority,
                     std::shared_ptr<std::atomic<bool>> cancel_token) {
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
        //     FrameReadWriteLock lock(frame_locks_[frame_index],
        //                             FrameLockType::Write);
        //     auto              &frame = frames_[frame_index];

        //     if (frame.status.load(std::memory_order_relaxed) !=
        //             LCBlockBufferPoolFrameStatus::Dirty ||
        //         frame.block_id.load(std::memory_order_relaxed) != block_id) {
        //         return;  // Skip if the frame is not dirty or does not match
        //     }
        //     frame.status.store(LCBlockBufferPoolFrameStatus::WriteInProgress,
        //                        std::memory_order_release);
        //     submit_flush_task(block_id, frame_index, priority, cancel_token);
        // }
    }

    void flush_all(LCTaskPriority priority) {
        // for (uint32_t i = 0; i < pool_size_; ++i) {
        //     LCBlockBufferPoolFrame &frame           = frames_[i];
        //     FrameStatus             expected_status = FrameStatus::Dirty;
        //     if (!frame.status.compare_exchange_strong(
        //             expected_status,
        //             FrameStatus::WriteInProgress,
        //             std::memory_order_acq_rel)) {
        //         continue;  // Retry if the frame is not in the expected state
        //     }

        //     submit_flush_task(frame.block_id, i, priority, nullptr);
        // }
    }

    void find_or_load_frame_with_version(uint32_t block_id, size_t &frame_index,
                                         uint64_t      &version,
                                         LCTaskPriority priority) {
        // frame_index = find_frame(block_id, priority);
        // version =
        // frames_[frame_index].version.load(std::memory_order_acquire);
    }

    void lock_frame(size_t frame_index, uint64_t &version,
                    FrameLockType &lock_type, FrameGuard &guard) {
        // guard = FrameGuard(&frames_[frame_index],
        //                    version,
        //                    frame_locks_[frame_index],
        //                    lock_type);
    }

    void lock_block(uint32_t block_id, FrameLockType &lock_type,
                    FrameGuard &guard, LCTaskPriority priority) {
        // size_t   frame_index = 0;
        // uint64_t version     = 0;

        // find_or_load_frame_with_version(block_id,
        //                                 frame_index,
        //                                 version,
        //                                 priority);
        // lock_frame(frame_index, version, lock_type, guard);
    }

private:

    class FrameReadWriteLock {
        using ReadLock  = std::shared_lock<std::shared_mutex>;
        using WriteLock = std::unique_lock<std::shared_mutex>;
    public:

        explicit FrameReadWriteLock(
            std::shared_ptr<std::shared_mutex> frame_lock,
            FrameLockType                      lock_type) :
            frame_lock_(std::move(frame_lock)) {
            if (lock_type == FrameLockType::Read) {
                read_lock_ = std::make_unique<ReadLock>(*frame_lock_);
            } else if (lock_type == FrameLockType::Write) {
                write_lock_ = std::make_unique<WriteLock>(*frame_lock_);
            } else {
                LC_ASSERT(false, "Invalid lock type for FrameReadWriteLock");
            }
        }

        ~FrameReadWriteLock() = default;

        FrameReadWriteLock(const FrameReadWriteLock &)            = delete;
        FrameReadWriteLock &operator=(const FrameReadWriteLock &) = delete;
        FrameReadWriteLock(FrameReadWriteLock &&)                 = delete;
        FrameReadWriteLock &operator=(FrameReadWriteLock &&)      = delete;

    private:
        std::shared_ptr<std::shared_mutex> frame_lock_;
        std::unique_ptr<ReadLock>          read_lock_;
        std::unique_ptr<WriteLock>         write_lock_;
    };

    uint32_t find_frame(uint32_t block_id, LCTaskPriority priority) {
        // while (true) {
        //     {
        //         std::shared_lock<std::shared_mutex> shared_lock(
        //             frame_map_mutex_);

        //         auto it = frame_map_.find(block_id);
        //         if (it != frame_map_.end()) {
        //             return it->second;  // Return existing frame index
        //         }
        //     }

        //     std::unique_lock<std::shared_mutex> write_lock(frame_map_mutex_);

        //     // Double-check locking to avoid ABA porblem, when we release the
        //     // the read lock, another thread may have inserted the block_id.
        //     auto it = frame_map_.find(block_id);
        //     if (it != frame_map_.end()) {
        //         return it->second;  // Return existing frame index
        //     }

        //     uint32_t    frame_index = evict_frame();
        //     auto       &frame       = frames_[frame_index];
        //     FrameStatus expected    = FrameStatus::Invalid;
        //     if (!frame.status.compare_exchange_strong(
        //             expected,
        //             FrameStatus::ReadInProgress,
        //             std::memory_order_acq_rel)) {
        //         continue;  // Retry if the frame is not in the expected state
        //     }

        //     std::shared_ptr<std::atomic<bool>> cancel_token =
        //         std::make_shared<std::atomic<bool>>(false);
        //     submit_read_task(block_id, frame_index, priority, cancel_token);

        //     frame.block_id.store(block_id, std::memory_order_relaxed);
        //     frame.ref_count.store(0, std::memory_order_relaxed);
        //     frame.usage_count.store(1, std::memory_order_relaxed);
        //     frame_map_[block_id] = frame_index;  // Insert into frame_map_
        //     return frame_index;                  // Return the new frame
        //     index
        // }
        // LC_ASSERT(false, "No reusable frame found, this should not happen");
        // return -1;                               // Should never reach here
    }

    // Clock sweep to find a reusable frame
    uint32_t evict_frame() {
        // while (true) {
        //     uint32_t frame_index =
        //         clock_hand_.fetch_add(1, std::memory_order_relaxed) %
        //         pool_size_;
        //     auto &frame = frames_[frame_index];

        //     FrameStatus status =
        //     frame.status.load(std::memory_order_acquire); uint8_t ref_count =
        //     frame.ref_count.load(std::memory_order_acquire); uint8_t
        //     usage_count =
        //         frame.usage_count.load(std::memory_order_acquire);

        //     if (ref_count > 0) {
        //         continue;
        //     }

        //     if (usage_count > 0) {
        //         frame.usage_count.fetch_sub(1, std::memory_order_relaxed);
        //         continue;
        //     }

        //     if (frame.status.load(std::memory_order_release) ==
        //         LCBlockBufferPoolFrameStatus::Invalid) {
        //         return frame_index;  // Found a reusable frame
        //     }

        //     FrameStatus expected_status = FrameStatus::ValidClean;
        //     if (!frame.status.compare_exchange_strong(
        //             expected_status,
        //             FrameStatus::Evicting,
        //             std::memory_order_acq_rel)) {
        //         continue;
        //     }

        //     uint64_t old_version =
        //         frame.version.load(std::memory_order_acquire);
        //     uint32_t old_block_id =
        //         frame.block_id.load(std::memory_order_relaxed);
        //     frame_map_.erase(old_block_id);
        //     frame.status.store(FrameStatus::Invalid,
        //     std::memory_order_release);
        //     frame.block_id.store(LC_BLOCK_ILLEGAL_ID,
        //                          std::memory_order_relaxed);
        //     frame.version.fetch_add(1, std::memory_order_relaxed);
        //     frame.usage_count.store(0, std::memory_order_relaxed);
        //     return frame_index;
        // }

        // // Should never reach here
        // LC_ASSERT(false, "No reusable frame found, this should not happen");
        // return -1;
    }

    // Background flush logic
    void background_flush_loop() {
        // while (running_.load(std::memory_order_release)) {
        //     wait_strategy_->wait_for(
        //         std::chrono::milliseconds(frame_interval_ms_));

        //     if (!running_.load(std::memory_order_acquire)) {
        //         break;  // Exit if the pool is stopped
        //     }

        //     flush_all(LCTaskPriority::Background);
        // }
    }

    void submit_flush_task(uint32_t block_id, size_t frame_index,
                           LCTaskPriority                     priority,
                           std::shared_ptr<std::atomic<bool>> cancel_token) {
        // LC_ASSERT(write_thread_pool_, "Write thread pool is not
        // initialized"); LCThreadPoolContextMetaData<LCTaskPriority> meta;
        // meta.listener_id = "block_buffer_pool";
        // lc_generate_trace_id(LCTraceTypeID::WriteTask, meta.trace_id);
        // meta.timestamp = std::time(nullptr);
        // meta.priority  = priority;

        // uint64_t frame_version =
        //     frames_[frame_index].version.load(std::memory_order_acquire);

        // auto task = std::make_shared<LCLambdaTask<std::function<void()>>>(
        //     [this, block_id, frame_index, frame_version]() {
        //     // TODO add log for failure and success
        //     if (frames_[frame_index].status.load(std::memory_order_acquire)
        //     !=
        //         LCBlockBufferPoolFrameStatus::WriteInProgress) {
        //         return;  // Skip if the frame is not in the expected state
        //     }

        //     if (frames_[frame_index].version.load(std::memory_order_acquire)
        //     !=
        //         frame_version) {
        //         return;  // Skip if the frame version has changed
        //     }

        //     block_device_->write_block(block_id, frames_[frame_index].block);
        //     frames_[frame_index].status.store(
        //         LCBlockBufferPoolFrameStatus::ValidClean,
        //         std::memory_order_release);
        // });
        // LCTreadPoolContextFactory<LCTaskPriority> context(meta,
        //                                                   task,
        //                                                   cancel_token);
        // write_thread_pool_->wait_and_submit_task(context);
    }

    void submit_read_task(uint32_t block_id, size_t frame_index,
                          LCTaskPriority                     priority,
                          std::shared_ptr<std::atomic<bool>> cancel_token) {
        // LC_ASSERT(read_thread_pool_, "Read thread pool is not initialized");
        // LCThreadPoolContextMetaData<LCTaskPriority> meta;
        // meta.listener_id = "block_buffer_pool";
        // lc_generate_trace_id(LCTraceTypeID::ReadTask, meta.trace_id);
        // meta.timestamp = std::time(nullptr);
        // meta.priority  = priority;
        // FrameIndexSlot slot {.frame_index = frame_index};
        // slot.ready.store(false, std::memory_order_relaxed);

        // uint64_t frame_version =
        //     frames_[frame_index].version.load(std::memory_order_acquire);

        // auto task = std::make_shared<LCLambdaTask<std::function<void()>>>(
        //     [this, block_id, &slot, frame_version]() {
        //     if (frames_[slot.frame_index].status.load(
        //             std::memory_order_acquire) !=
        //         LCBlockBufferPoolFrameStatus::ReadInProgress) {
        //         return;  // Skip if the frame is not in the expected state
        //     }
        //     if (frames_[slot.frame_index].version.load(
        //             std::memory_order_acquire) != frame_version) {
        //         return;  // Skip if the frame version has changed
        //     }
        //     block_device_->read_block(block_id,
        //                               frames_[slot.frame_index].block);
        //     frames_[slot.frame_index].status.store(
        //         LCBlockBufferPoolFrameStatus::ValidClean,
        //         std::memory_order_release);
        //     slot.ready.store(true, std::memory_order_release);
        // });
        // LCTreadPoolContextFactory<LCTaskPriority> context(meta,
        //                                                   task,
        //                                                   cancel_token);
        // read_thread_pool_->wait_and_submit_task(context);

        // while (!slot.ready.load(std::memory_order_acquire)) {
        //     if (cancel_token &&
        //     cancel_token->load(std::memory_order_acquire)) {
        //         return;
        //     }
        //     wait_strategy_->wait_for(
        //         std::chrono::milliseconds(frame_interval_ms_));
        // }
    }

    void init_resources() {
        // frames_ = std::make_unique<Frame[]>(pool_size_);
        // frame_locks_ =
        //     std::make_unique<std::shared_ptr<std::shared_mutex>[]>(pool_size_);
        // wait_strategy_ = std::make_unique<LCConditionVariableWaitStrategy>();
        // for (uint32_t i = 0; i < pool_size_; ++i) {
        //     frame_locks_[i] = std::make_shared<std::shared_mutex>();
        //     frames_[i].status.store(LCBlockBufferPoolFrameStatus::Invalid,
        //                             std::memory_order_relaxed);
        //     frames_[i].ref_count   = 0;
        //     frames_[i].usage_count = 0;
        //     frames_[i].block_id    = LC_BLOCK_ILLEGAL_ID;
        //     block_clear(&frames_[i].block);
        // }
    }

    void free_resources() {
        // frames_      = nullptr;
        // frame_locks_ = nullptr;
    }

    // LCBlockManager         *block_manager_;
    // LCBlockDevice           *block_device_;
    // std::unique_ptr<Frame[]> frames_;
    // uint32_t                 pool_size_;
    // uint32_t                 frame_interval_ms_;
    // std::unordered_map<uint32_t, uint32_t>
    //     frame_map_;   // Maps block_id to frame index

    // std::atomic<uint32_t>
    //     clock_hand_;  // For clock algorithm, store the index of the next
    //                   // frame to check, this should be atomic

    // // Concurrency
    // std::shared_mutex frame_map_mutex_;  // Mutex for frame_map_
    // std::thread       background_thread_;
    // std::atomic<bool> running_ {false};
    // // std::atomic_flag *frame_locks_;      // Spin locks for each frame
    // std::unique_ptr<std::shared_ptr<std::shared_mutex>[]> frame_locks_;

    // std::unique_ptr<LCWaitStrategyBase>           wait_strategy_;
    // std::shared_ptr<LCThreadPool<LCTaskPriority>> write_thread_pool_;
    // std::shared_ptr<LCThreadPool<LCTaskPriority>> read_thread_pool_;
    // static constexpr const char *thread_name_ = "block_buffer_pool";
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_BUFFER_H
