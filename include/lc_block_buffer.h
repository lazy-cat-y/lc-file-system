#ifndef LC_BLOCK_BUFFER_H
#define LC_BLOCK_BUFFER_H

#include <sys/types.h>

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
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
 * WriteInProgress can be read but can't be write, normally it will wait the
 * write finish
 * Dirty can be read and writen
 * ValidClean → Evicting → Invalid
 */
enum class BlockBufferPoolFrameStatus {
    Invalid,
    ReadInProgress,
    ValidClean,
    Dirty,
    FlushInProgress,
    Evicting  // for eviction
};

enum class BlockFrameGuardLockType {
    Read,
    Write,
};

typedef struct BlockBufferPoolFrame {
    std::atomic<BlockBufferPoolFrameStatus>
                         status;     // Indicates if the frame has been modified
    std::atomic<uint8_t> ref_count;  // Reference count for the frame
    std::atomic<uint8_t>
        usage_count;                 // Usage count for the frame, used for LRU
    std::atomic<uint64_t> version;   // Status flags for the frame
    std::atomic<uint32_t> block_id;  // The ID of the block this frame holds
    Block               block;     // The block data
} BlockBufferPoolFrame;

LC_CONSTEXPR inline void __lc_add_lc_block_usage_count(
    std::atomic<uint8_t> &usage_count) {
    increment_usage_count_if_not_max(usage_count, 5);
}

struct BlockFrameGuard {
    using Frame              = BlockBufferPoolFrame;
    using ReadLock           = std::shared_lock<std::shared_mutex>;
    using WriteLock          = std::unique_lock<std::shared_mutex>;
    using FrameGuardLockType = BlockFrameGuardLockType;

    std::shared_ptr<std::shared_mutex> lock;   // Shared mutex for the frame
    ReadLock                           read_lock;
    WriteLock                          write_lock;
    std::shared_ptr<Frame>             frame;  // Pointer to the frame
    uint64_t                           version;
    FrameGuardLockType                 lock_type;

    BlockFrameGuard(std::shared_ptr<Frame> frame, uint64_t version,
                      std::shared_ptr<std::shared_mutex> lock,
                      FrameGuardLockType                 lock_type) :
        frame(std::move(frame)),
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

    BlockFrameGuard() = default;

    BlockFrameGuard(const BlockFrameGuard &)            = delete;
    BlockFrameGuard &operator=(const BlockFrameGuard &) = delete;

    BlockFrameGuard(BlockFrameGuard &&other) :
        frame(std::move(other.frame)),
        lock(std::move(other.lock)),
        read_lock(std::move(other.read_lock)),
        write_lock(std::move(other.write_lock)),
        version(other.version),
        lock_type(other.lock_type) {
        other.frame = nullptr;  // Prevent double decrement
        other.lock  = nullptr;  // Prevent double decrement
    }

    BlockFrameGuard &operator=(BlockFrameGuard &&other) {
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
    ~BlockFrameGuard() {
        if (frame) {
            frame->ref_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    bool is_valid() const {
        return frame &&
               frame->version.load(std::memory_order_acquire) == version &&
               frame->status.load(std::memory_order_acquire) !=
                   BlockBufferPoolFrameStatus::Invalid;
    }

    void mark_dirty() {
        if (frame) {
            frame->status.store(BlockBufferPoolFrameStatus::Dirty,
                                std::memory_order_release);
        }
    }
};

class BlockBufferPool {
    using Frame         = BlockBufferPoolFrame;
    using FrameStatus   = BlockBufferPoolFrameStatus;
    using FrameGuard    = BlockFrameGuard;
    using FrameLockType = BlockFrameGuardLockType;
    using ThreadPoolContextMetaData =
        ThreadPoolContextMetaData<LCTaskPriority>;
    using ContextFactory  = TreadPoolContextFactory<LCTaskPriority>;
    using CancelTokenType = std::shared_ptr<std::atomic<bool>>;

    enum class FrameIndexSlotStatus {
        Processing,
        StatusUnexpected,
        VersionUnexpected,
        Cancelled,
        Ready,
    };

    struct FrameIndexSlot {
        std::atomic<FrameIndexSlotStatus> ready;
        size_t                            frame_index;
    };

    enum class FrameAcquireResult {
        ReadThreadPoolClose,
        TaskCancelled,
        Success,
        UnknownError,
    };

    enum class TaskSubmitResult {
        Success,
        Cancelled,
        ThreadPoolClose,
        UnknownError,
    };

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
public:

    BlockBufferPool() = delete;

    ~BlockBufferPool() {
        LC_ASSERT(!running_.load(std::memory_order_acquire),
                  "Cannot destruct while running");
        free_resources();
    }

    BlockBufferPool(const BlockBufferPool &)            = delete;
    BlockBufferPool &operator=(const BlockBufferPool &) = delete;
    BlockBufferPool(BlockBufferPool &&)                 = delete;
    BlockBufferPool &operator=(BlockBufferPool &&)      = delete;

    LC_EXPLICIT BlockBufferPool(
        std::shared_ptr<BlockDevice> block_device, size_t pool_size,
        std::shared_ptr<ThreadPool<LCTaskPriority>> write_thread_pool,
        std::shared_ptr<ThreadPool<LCTaskPriority>> read_thread_pool) :
        block_device_(std::move(block_device)),
        frame_pool_size_(pool_size),
        bg_thread_pool_(std::move(write_thread_pool)),
        fg_thread_pool_(std::move(read_thread_pool)) {
        init_resources();
    }

    //     block_device_(block_device),
    //     pool_size_(pool_size),
    //     frame_interval_ms_(frame_interval_ms),
    //     clock_hand_(0),
    //     write_thread_pool_(std::move(write_thread_pool)),
    //     read_thread_pool_(std::move(read_thread_pool)) {
    //     init_resources();
    // }

    void start() {
        bool expected = false;
        if (running_.compare_exchange_strong(expected, true)) {
            background_thread_ =
                std::thread(&BlockBufferPool::background_flush_loop, this);
        }
    }

    void stop() {
        bool expected = true;
        if (running_.compare_exchange_strong(expected, false)) {
            wait_strategy_->notify_all();
            if (background_thread_.joinable()) {
                background_thread_.join();
            }
            flush_all(LCTaskPriority::High, nullptr);
        }
    }

    // This function copies the contents, ref_count is not incremented
    void read_block(uint32_t block_id, Block &block, LCTaskPriority priority,
                    CancelTokenType cancel_token) {
        while (true) {
            if (task_is_cancelled(cancel_token)) {
                return;  // Exit if the task is cancelled
            }
            size_t             frame_index = LC_BLOCK_ILLEGAL_ID;
            FrameAcquireResult result =
                acquire_frame(block_id, priority, cancel_token, frame_index);
            if (result == FrameAcquireResult::ReadThreadPoolClose ||
                result == FrameAcquireResult::TaskCancelled) {
                return;  // Exit if the read thread pool is closed
            } else if (result == FrameAcquireResult::UnknownError) {
                return;
            }
            {
                FrameReadWriteLock lock(frame_locks_[frame_index],
                                        FrameLockType::Read);

                Frame      &frame = *frame_pool_[frame_index];
                FrameStatus status =
                    frame.status.load(std::memory_order_relaxed);
                if ((status != FrameStatus::ValidClean &&
                     status != FrameStatus::Dirty) ||
                    frame.block_id.load(std::memory_order_relaxed) !=
                        block_id) {
                    continue;
                }
                __lc_add_lc_block_usage_count(frame.usage_count);
                lc_memcpy(&block, &frame.block, DEFAULT_BLOCK_SIZE);
                return;
            }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
    }

    void read_block(uint32_t block_id, LCTaskPriority priority,
                    CancelTokenType cancel_token, void *data, uint32_t size,
                    uint32_t offset = 0) {
        LC_ASSERT(size > 0, "Size must be positive");
        LC_ASSERT(offset >= 0, "Offset must be non-negative");
        LC_ASSERT(size <= DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");

        while (true) {
            LC_ASSERT(data != nullptr, "Data pointer cannot be null");
            if (task_is_cancelled(cancel_token)) {
                return;  // Exit if the task is cancelled
            }
            size_t             frame_index = LC_BLOCK_ILLEGAL_ID;
            FrameAcquireResult result =
                acquire_frame(block_id, priority, cancel_token, frame_index);
            if (result == FrameAcquireResult::ReadThreadPoolClose ||
                result == FrameAcquireResult::TaskCancelled) {
                return;  // Exit if the read thread pool is closed
            } else if (result == FrameAcquireResult::UnknownError) {
                return;
            }
            {
                FrameReadWriteLock lock(frame_locks_[frame_index],
                                        FrameLockType::Read);
                Frame             &frame = *frame_pool_[frame_index];
                auto status = frame.status.load(std::memory_order_relaxed);
                if ((status != FrameStatus::ValidClean &&
                     status != FrameStatus::Dirty) ||
                    frame.block_id.load(std::memory_order_relaxed) !=
                        block_id) {
                    continue;  // Retry if the frame is not valid or does not
                               // match
                }
                __lc_add_lc_block_usage_count(frame.usage_count);
                lc_memcpy(static_cast<uint8_t *>(data),
                          block_as(&frame.block) + offset,
                          size);
                return;
            }
        }
        LC_ASSERT(false, "Block ID not found, this should not happen");
    }

    void write_block(uint32_t block_id, LCTaskPriority priority,
                     CancelTokenType cancel_token, const void *data,
                     uint32_t size, uint32_t offset = 0) {
        LC_ASSERT(size <= DEFAULT_BLOCK_SIZE - offset,
                  "Size exceeds block size minus offset");
        while (true) {
            LC_ASSERT(data != nullptr, "Data pointer cannot be null");
            if (task_is_cancelled(cancel_token)) {
                return;  // Exit if the task is cancelled
            }
            size_t             frame_index = LC_BLOCK_ILLEGAL_ID;
            FrameAcquireResult result =
                acquire_frame(block_id, priority, cancel_token, frame_index);
            if (result == FrameAcquireResult::ReadThreadPoolClose ||
                result == FrameAcquireResult::TaskCancelled) {
                return;  // Exit if the read thread pool is closed
            } else if (result == FrameAcquireResult::UnknownError) {
                return;
            }
            {
                FrameReadWriteLock lock(frame_locks_[frame_index],
                                        FrameLockType::Write);
                Frame             &frame = *frame_pool_[frame_index];

                FrameStatus status =
                    frame.status.load(std::memory_order_relaxed);

                if ((status != FrameStatus::ValidClean &&
                     status != FrameStatus::Dirty) ||
                    frame.block_id.load(std::memory_order_relaxed) !=
                        block_id) {
                    continue;  // Retry if the frame is not valid or does not
                               // match
                }

                lc_memcpy(block_as(&frame.block) + offset, data, size);
                frame.status.store(BlockBufferPoolFrameStatus::Dirty,
                                   std::memory_order_release);
                __lc_add_lc_block_usage_count(frame.usage_count);
                return;
            }
        }
        LC_ASSERT(false, "Failed to write block");
    }

    void flush_block(uint32_t block_id, LCTaskPriority priority,
                     CancelTokenType cancel_token) {
        if (task_is_cancelled(cancel_token)) {
            return;  // Exit if the task is cancelled
        }
        size_t frame_index = LC_BLOCK_ILLEGAL_ID;
        {
            std::shared_lock<std::shared_mutex> shared_lock(frame_map_lock_);
            auto                                it = frame_map_.find(block_id);
            if (it == frame_map_.end()) {
                return;
            }
            frame_index = it->second;
        }
        if (task_is_cancelled(cancel_token)) {
            return;  // Exit if the task is cancelled
        }
        LC_ASSERT(frame_index != LC_BLOCK_ILLEGAL_ID,
                  "Frame index not found for block ID");
        {
            FrameReadWriteLock lock(frame_locks_[frame_index],
                                    FrameLockType::Write);
            auto              &frame = *frame_pool_[frame_index];

            if (frame.status.load(std::memory_order_relaxed) !=
                    BlockBufferPoolFrameStatus::Dirty ||
                frame.block_id.load(std::memory_order_relaxed) != block_id) {
                return;  // Skip if the frame is not dirty or does not match
            }
            frame.status.store(BlockBufferPoolFrameStatus::FlushInProgress,
                               std::memory_order_release);
            if (task_is_cancelled(cancel_token)) {
                return;  // Exit if the task is cancelled
            }
            submit_flush_task(block_id,
                              frame_index,
                              priority,
                              TraceTypeID::FlushTask,
                              cancel_token);
        }
    }

    void flush_all(LCTaskPriority priority, CancelTokenType cancel_token) {
        for (uint32_t i = 0; i < frame_pool_size_; ++i) {
            Frame      &frame           = *frame_pool_[i];
            FrameStatus expected_status = FrameStatus::Dirty;
            if (task_is_cancelled(cancel_token)) {
                return;  // Exit if the task is cancelled
            }
            if (!frame.status.compare_exchange_strong(
                    expected_status,
                    FrameStatus::FlushInProgress,
                    std::memory_order_acq_rel)) {
                continue;  // Retry if the frame is not in the expected state
            }

            submit_flush_task(frame.block_id,
                              i,
                              priority,
                              TraceTypeID::BackgroundFlushTask,
                              cancel_token);
        }
    }

    void find_or_load_frame_with_version(uint32_t block_id, size_t &frame_index,
                                         uint64_t       &version,
                                         LCTaskPriority  priority,
                                         CancelTokenType cancel_token) {
        // frame_index = acquire_frame(block_id, priority, cancel_token);

        frame_index = LC_BLOCK_ILLEGAL_ID;
        FrameAcquireResult result =
            acquire_frame(block_id, priority, cancel_token, frame_index);
        if (result == FrameAcquireResult::ReadThreadPoolClose ||
            result == FrameAcquireResult::TaskCancelled) {
            return;  // Exit if the read thread pool is closed
        } else if (result == FrameAcquireResult::UnknownError) {
            return;
        }
        if (frame_index == LC_BLOCK_ILLEGAL_ID) {
            return;
        }
        version =
            frame_pool_[frame_index]->version.load(std::memory_order_acquire);
    }

    void lock_frame(size_t frame_index, uint64_t &version,
                    FrameLockType &lock_type, FrameGuard &guard) {
        guard = FrameGuard(frame_pool_[frame_index],
                           version,
                           frame_locks_[frame_index],
                           lock_type);
    }

    void lock_block(uint32_t block_id, FrameLockType &lock_type,
                    FrameGuard &guard, LCTaskPriority priority,
                    CancelTokenType cancel_token) {
        size_t   frame_index = 0;
        uint64_t version     = 0;

        find_or_load_frame_with_version(block_id,
                                        frame_index,
                                        version,
                                        priority,
                                        cancel_token);
        if (frame_index == LC_BLOCK_ILLEGAL_ID) {
            return;
        }
        if (task_is_cancelled(cancel_token)) {
            return;  // Exit if the task is cancelled
        }
        lock_frame(frame_index, version, lock_type, guard);
        return;
    }

private:

    FrameAcquireResult acquire_frame(uint32_t block_id, LCTaskPriority priority,
                                     CancelTokenType cancel_token,
                                     size_t         &result_frame_index) {
        while (true) {
            if (task_is_cancelled(cancel_token)) {
                return FrameAcquireResult::TaskCancelled;  // Exit if the task
                                                           // is cancelled
            }

            {
                std::shared_lock<std::shared_mutex> shared_lock(
                    frame_map_lock_);

                auto it = frame_map_.find(block_id);
                if (it != frame_map_.end()) {
                    result_frame_index =
                        it->second;  // Return existing frame index
                    return FrameAcquireResult::Success;
                }
            }

            if (task_is_cancelled(cancel_token)) {
                return FrameAcquireResult::TaskCancelled;  // Exit if the task
                                                           // is cancelled
            }
            // Not found, write map
            std::unique_lock<std::shared_mutex> write_lock(frame_map_lock_);
            // Double-check locking to avoid ABA problem, when we release the
            // read lock, another thread may have inserted the block_id.
            auto it = frame_map_.find(block_id);
            if (it != frame_map_.end()) {
                result_frame_index = it->second;
                return FrameAcquireResult::Success;
            }

            // check whether existing invalid frame can be reused
            for (size_t i = 0; i < frame_pool_size_; ++i) {
                Frame &frame = *frame_pool_[i];
                // Since the frame is invalid, it is unnecessary to check the
                // other values.
                FrameStatus expected_status = FrameStatus::Invalid;
                if (frame.status.compare_exchange_strong(
                        expected_status,
                        FrameStatus::ReadInProgress,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    uint32_t old_block_id =
                        frame.block_id.load(std::memory_order_relaxed);
                    frame_map_.erase(old_block_id);  // Remove from frame_map_

                    frame.block_id.store(block_id, std::memory_order_relaxed);
                    frame.ref_count.store(0, std::memory_order_relaxed);
                    frame.usage_count.store(1, std::memory_order_relaxed);
                    frame.version.fetch_add(1, std::memory_order_relaxed);
                    frame_map_[block_id] = i;

                    write_lock.unlock();

                    TaskSubmitResult result =
                        submit_read_task(block_id, i, priority, cancel_token);
                    switch (result) {
                        case TaskSubmitResult::Success :
                            // Task submitted successfully
                            break;
                        case TaskSubmitResult::Cancelled :
                            // Task was cancelled
                            return FrameAcquireResult::TaskCancelled;
                        case TaskSubmitResult::ThreadPoolClose :
                            // Thread pool is closed
                            return FrameAcquireResult::ReadThreadPoolClose;
                        case TaskSubmitResult::UnknownError :
                        default                             : return FrameAcquireResult::UnknownError;
                    }
                    // if (!ok) {
                    //     // continue;
                    //     return LC_BLOCK_ILLEGAL_ID;  // Indicate failure
                    // }
                    result_frame_index = i;  // Return the new frame index
                    return FrameAcquireResult::Success;
                }
            }

            if (task_is_cancelled(cancel_token)) {
                result_frame_index = LC_BLOCK_ILLEGAL_ID;
                return FrameAcquireResult::TaskCancelled;  // Exit if the task
                                                           // is cancelled
                // return LC_BLOCK_ILLEGAL_ID;  // Exit if the task is cancelled
            }

            // use clock algorithm to find a reusable frame
            size_t frame_index = pick_victim_and_mark_evicting_unlocked();
            LC_ASSERT(frame_index >= 0 && frame_index < frame_pool_size_,
                      "Invalid frame index found during eviction");

            Frame   &frame = *frame_pool_[frame_index];
            uint32_t old_block_id =
                frame.block_id.load(std::memory_order_relaxed);
            frame_map_.erase(old_block_id);  // Remove from frame_map_

            frame.block_id.store(block_id, std::memory_order_relaxed);
            frame.ref_count.store(0, std::memory_order_relaxed);
            frame.usage_count.store(1, std::memory_order_relaxed);
            frame.version.fetch_add(1, std::memory_order_relaxed);

            frame_map_[block_id] = frame_index;  // Insert into frame_map_

            frame.status.store(FrameStatus::ReadInProgress,
                               std::memory_order_release);

            write_lock.unlock();
            TaskSubmitResult result =
                submit_read_task(block_id, frame_index, priority, cancel_token);
            switch (result) {
                case TaskSubmitResult::Success :
                    // Task submitted successfully
                    break;
                case TaskSubmitResult::Cancelled :
                    // Task was cancelled
                    return FrameAcquireResult::TaskCancelled;
                case TaskSubmitResult::ThreadPoolClose :
                    // Thread pool is closed
                    return FrameAcquireResult::ReadThreadPoolClose;
                case TaskSubmitResult::UnknownError :
                default                             : return FrameAcquireResult::UnknownError;
            }
            result_frame_index = frame_index;  // Return the new frame
            return FrameAcquireResult::Success;
        }
        LC_ASSERT(false, "No reusable frame found, this should not happen");
        return FrameAcquireResult::UnknownError;  // Should not reach here
    }

    // Clock sweep to find a reusable frame
    size_t pick_victim_and_mark_evicting_unlocked() {
        while (true) {
            size_t frame_index =
                clock_hand_.fetch_add(1, std::memory_order_relaxed) %
                frame_pool_size_;
            Frame &frame = *frame_pool_[frame_index];

            if (frame.ref_count.load(std::memory_order_acquire) > 0) {
                continue;  // Skip if the frame is still in use
            }

            uint8_t usage_count =
                frame.usage_count.load(std::memory_order_acquire);
            if (usage_count > 0) {
                frame.usage_count.fetch_sub(1, std::memory_order_relaxed);
                continue;  // Skip if the frame is still in use
            }

            FrameStatus expected_status =
                FrameStatus::ValidClean;  // Check if the frame is valid
            if (!frame.status.compare_exchange_strong(
                    expected_status,
                    FrameStatus::Evicting,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                continue;  // Retry if the frame is not in the expected state
            }
            return frame_index;
        }
        LC_ASSERT(false, "No reusable frame found, this should not happen");
        return 0;
    }

    // Background flush logic
    void background_flush_loop() {
        while (running_.load(std::memory_order_acquire)) {
            wait_strategy_->wait_for(frame_interval_ms_);

            if (!running_.load(std::memory_order_acquire)) {
                break;  // Exit if the pool is stopped
            }

            CancelTokenType cancel_token =
                std::make_shared<std::atomic<bool>>(false);

            flush_all(LCTaskPriority::Background, cancel_token);

            if (!running_.load(std::memory_order_acquire)) {
                cancel_token->store(true, std::memory_order_release);
                break;  // Exit if the pool is stopped
            }
        }
    }

    void submit_flush_task(uint32_t block_id, size_t frame_index,
                           LCTaskPriority priority, TraceTypeID trace_type,
                           CancelTokenType cancel_token) {
        LC_ASSERT(bg_thread_pool_, "Write thread pool is not initialized");
        ThreadPoolContextMetaData metadata {};
        metadata.listener_id = thread_name_;
        generate_trace_id(trace_type, metadata.trace_id);
        metadata.timestamp = std::time(nullptr);
        metadata.priority  = priority;

        uint64_t frame_version =
            frame_pool_[frame_index]->version.load(std::memory_order_acquire);

        std::shared_ptr<FrameIndexSlot> slot =
            std::make_shared<FrameIndexSlot>();
        slot->frame_index = frame_index;
        slot->ready.store(FrameIndexSlotStatus::Processing,
                          std::memory_order_relaxed);

        auto task = std::make_shared<LambdaTask<std::function<void()>>>(
            [this, block_id, slot, frame_version, cancel_token]() {
            if (task_is_cancelled(cancel_token)) {
                if (frame_pool_[slot->frame_index]->status.load(
                        std::memory_order_acquire) ==
                        FrameStatus::FlushInProgress &&
                    frame_pool_[slot->frame_index]->version.load(
                        std::memory_order_acquire) == frame_version) {
                    // If the frame is in flush in progress, we need to
                    // cancel the flush
                    slot->ready.store(FrameIndexSlotStatus::Cancelled,
                                      std::memory_order_release);
                }
                frame_pool_[slot->frame_index]->status.store(
                    FrameStatus::Dirty,
                    std::memory_order_release);
                return;
            }
            if (frame_pool_[slot->frame_index]->status.load(
                    std::memory_order_acquire) !=
                FrameStatus::FlushInProgress) {
                slot->ready.store(FrameIndexSlotStatus::StatusUnexpected,
                                  std::memory_order_release);
                return;
            }

            if (frame_pool_[slot->frame_index]->version.load(
                    std::memory_order_acquire) != frame_version) {
                slot->ready.store(FrameIndexSlotStatus::VersionUnexpected,
                                  std::memory_order_release);
                return;
            }

            if (frame_pool_[slot->frame_index]->block_id.load(
                    std::memory_order_acquire) != block_id) {
                slot->ready.store(FrameIndexSlotStatus::StatusUnexpected,
                                  std::memory_order_release);
                return;  // Skip if the block ID has changed
            }

            block_device_->write_block(block_id,
                                       frame_pool_[slot->frame_index]->block);

            if (frame_pool_[slot->frame_index]->status.load(
                    std::memory_order_acquire) !=
                FrameStatus::FlushInProgress) {
                slot->ready.store(FrameIndexSlotStatus::StatusUnexpected,
                                  std::memory_order_release);
                return;
            }

            if (frame_pool_[slot->frame_index]->version.load(
                    std::memory_order_acquire) != frame_version) {
                slot->ready.store(FrameIndexSlotStatus::VersionUnexpected,
                                  std::memory_order_release);
                return;  // Skip if the frame version has changed
            }

            frame_pool_[slot->frame_index]->status.store(
                FrameStatus::ValidClean,
                std::memory_order_release);
            slot->ready.store(FrameIndexSlotStatus::Ready,
                              std::memory_order_release);
        });

        ContextFactory context_factory(metadata, task);
        bg_thread_pool_->wait_and_submit_task(context_factory);
    }

    LC_NODISCARD TaskSubmitResult
    submit_read_task(uint32_t block_id, size_t frame_index,
                     LCTaskPriority priority, CancelTokenType cancel_token) {
        LC_ASSERT(fg_thread_pool_, "Read thread pool is not initialized");
        LC_ASSERT(frame_index < frame_pool_size_,
                  "Frame index out of bounds for block buffer pool");
        LC_ASSERT(block_id != LC_BLOCK_ILLEGAL_ID,
                  "Block ID cannot be illegal in read task submission");
        ThreadPoolContextMetaData meta;
        meta.listener_id = "block_buffer_pool";
        generate_trace_id(TraceTypeID::ReadTask, meta.trace_id);
        meta.timestamp = std::time(nullptr);
        meta.priority  = priority;
        std::shared_ptr<FrameIndexSlot> slot =
            std::make_shared<FrameIndexSlot>();
        slot->frame_index = frame_index;
        // slot.ready.store(false, std::memory_order_relaxed);
        slot->ready.store(FrameIndexSlotStatus::Processing,
                          std::memory_order_relaxed);

        uint64_t frame_version =
            frame_pool_[frame_index]->version.load(std::memory_order_acquire);

        auto task = std::make_shared<LambdaTask<std::function<void()>>>(
            [this, block_id, slot, frame_version, cancel_token]() {
            if (task_is_cancelled(cancel_token)) {
                slot->ready.store(FrameIndexSlotStatus::Cancelled,
                                  std::memory_order_release);
                return;  // Exit if the task is cancelled
            }
            if (frame_pool_[slot->frame_index]->status.load(
                    std::memory_order_acquire) != FrameStatus::ReadInProgress) {
                slot->ready.store(FrameIndexSlotStatus::StatusUnexpected,
                                  std::memory_order_release);
                return;
            }

            if (frame_pool_[slot->frame_index]->version.load(
                    std::memory_order_acquire) != frame_version) {
                slot->ready.store(FrameIndexSlotStatus::VersionUnexpected,
                                  std::memory_order_release);
                return;  // Skip if the frame version has changed
            }

            block_device_->read_block(block_id,
                                      frame_pool_[slot->frame_index]->block);
            // frame_pool_[slot.frame_index].status.store(
            //     FrameStatus::ValidClean,
            //     std::memory_order_release);
            slot->ready.store(FrameIndexSlotStatus::Ready,
                              std::memory_order_release);
        });

        ContextFactory context_factory(meta, task);
        bool           submit_success =
            fg_thread_pool_->wait_and_submit_task(context_factory);

        if (!submit_success) {
            slot->ready.store(FrameIndexSlotStatus::Cancelled,
                              std::memory_order_release);
            if (fg_thread_pool_->is_draining() ||
                fg_thread_pool_->is_stopped()) {
                return TaskSubmitResult::ThreadPoolClose;
            }
            return TaskSubmitResult::UnknownError;
        }

        while (slot->ready.load(std::memory_order_acquire) ==
               FrameIndexSlotStatus::Processing) {
            wait_strategy_->wait_for(
                std::chrono::milliseconds(frame_interval_ms_));
        }

        FrameIndexSlotStatus status =
            slot->ready.load(std::memory_order_acquire);
        if (status == FrameIndexSlotStatus::Ready) {
            if (frame_pool_[frame_index]->status.load(
                    std::memory_order_acquire) == FrameStatus::ReadInProgress &&
                frame_pool_[frame_index]->version.load(
                    std::memory_order_acquire) == frame_version) {
                frame_pool_[frame_index]->status.store(
                    FrameStatus::ValidClean,
                    std::memory_order_release);
                return TaskSubmitResult::Success;
            }
        }
        {
            std::unique_lock lock(frame_map_lock_);
            frame_map_.erase(block_id);  // 删除占位映射
        }
        // If the task was cancelled, we need to reset the frame status
        frame_pool_[frame_index]->status.store(FrameStatus::Invalid,
                                               std::memory_order_release);
        frame_pool_[frame_index]->block_id.store(LC_BLOCK_ILLEGAL_ID,
                                                 std::memory_order_release);
        frame_pool_[frame_index]->ref_count.store(0, std::memory_order_release);
        frame_pool_[frame_index]->usage_count.store(0,
                                                    std::memory_order_release);
        frame_pool_[frame_index]->version.fetch_add(1,
                                                    std::memory_order_release);
        return TaskSubmitResult::Cancelled;
    }

    void init_resources() {
        frame_pool_ =
            std::make_unique<std::shared_ptr<Frame>[]>(frame_pool_size_);
        frame_locks_ = std::make_unique<std::shared_ptr<std::shared_mutex>[]>(
            frame_pool_size_);
        wait_strategy_ = std::make_unique<ConditionVariableWaitStrategy>();
        for (size_t i = 0; i < frame_pool_size_; ++i) {
            frame_locks_[i] = std::make_shared<std::shared_mutex>();
            frame_pool_[i]  = std::make_shared<Frame>();
            frame_pool_[i]->status.store(BlockBufferPoolFrameStatus::Invalid,
                                         std::memory_order_relaxed);
            frame_pool_[i]->ref_count.store(0, std::memory_order_relaxed);
            frame_pool_[i]->version.store(0, std::memory_order_relaxed);
            frame_pool_[i]->usage_count.store(0, std::memory_order_relaxed);
            frame_pool_[i]->block_id.store(LC_BLOCK_ILLEGAL_ID,
                                           std::memory_order_relaxed);
            block_clear(&frame_pool_[i]->block);
        }
    }

    void free_resources() {}

    size_t                                                frame_pool_size_;
    std::unique_ptr<std::shared_ptr<Frame>[]>             frame_pool_;
    std::unique_ptr<std::shared_ptr<std::shared_mutex>[]> frame_locks_;
    std::unordered_map<uint32_t, uint32_t>
                      frame_map_;  // Maps block_id to frame index
    std::shared_mutex frame_map_lock_;

    std::atomic<size_t>
        clock_hand_;  // For clock algorithm, store the index of the next

    std::shared_ptr<BlockDevice>                block_device_;
    std::unique_ptr<WaitStrategyBase>           wait_strategy_;
    std::shared_ptr<ThreadPool<LCTaskPriority>> bg_thread_pool_;
    std::shared_ptr<ThreadPool<LCTaskPriority>> fg_thread_pool_;
    static constexpr const char *thread_name_ = "block_buffer_pool";

    // Buffer pool thread
    std::thread                                background_thread_;
    std::atomic<bool>                          running_;
    static constexpr std::chrono::milliseconds frame_interval_ms_ =
        std::chrono::milliseconds(1000);
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_BUFFER_H
