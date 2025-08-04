

#ifndef LC_MPMC_QUEUE_H
#define LC_MPMC_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "lc_configs.h"
#include "lc_exception.h"
#include "lc_memory.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

// Based on Vyukov's MPMC queue implementation
// TODO finish the description of the algorithm

#define __LC_CACHE_LINE_SIZE 64
typedef char __lc_cacheline_pad_t[__LC_CACHE_LINE_SIZE];

template <typename T>
class LCMPMCQueue {
public:

    LCMPMCQueue(size_t buffer_size) : buffer_mask_(buffer_size - 1) {
        LC_ASSERT(buffer_size >= 2 && (buffer_size & (buffer_size - 1)) == 0,
                  "Buffer size must be a power of two and at least 2.");
        // buffer_ = lc_alloc_array<__lc_mpmc_queue_cell_t>(buffer_size);
        buffer_ = lc_construct_array<__lc_mpmc_queue_cell_t>(buffer_size);
        for (size_t i = 0; i < buffer_size; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_index_.store(0, std::memory_order_release);
        dequeue_index_.store(0, std::memory_order_release);
    }

    ~LCMPMCQueue() {
        lc_destroy_array(buffer_, buffer_mask_ + 1);
    }

    LCMPMCQueue()                               = delete;
    LCMPMCQueue(const LCMPMCQueue &)            = delete;
    LCMPMCQueue &operator=(const LCMPMCQueue &) = delete;
    LCMPMCQueue(LCMPMCQueue &&)                 = delete;
    LCMPMCQueue &operator=(LCMPMCQueue &&)      = delete;

    bool enqueue(const T &item) {
        __lc_mpmc_queue_cell_t *cell;
        size_t pos = enqueue_index_.load(std::memory_order_relaxed);
        while (true) {
            cell          = &buffer_[pos & buffer_mask_];
            size_t   seq  = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enqueue_index_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue is full
            } else {
                // Wait for the cell to be available
                pos = enqueue_index_.load(std::memory_order_relaxed);
            }
        }
        cell->data = item;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool enqueue(T &&item) {
        __lc_mpmc_queue_cell_t *cell;
        size_t pos = enqueue_index_.load(std::memory_order_relaxed);
        while (true) {
            cell          = &buffer_[pos & buffer_mask_];
            size_t   seq  = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enqueue_index_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue is full
            } else {
                // Wait for the cell to be available
                pos = enqueue_index_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool dequeue(T &item) {
        __lc_mpmc_queue_cell_t *cell;
        size_t pos = dequeue_index_.load(std::memory_order_relaxed);
        while (true) {
            cell          = &buffer_[pos & buffer_mask_];
            size_t   seq  = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
            if (diff == 0) {
                if (dequeue_index_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue is empty
            } else {
                // Wait for the cell to be available
                pos = dequeue_index_.load(std::memory_order_relaxed);
            }
        }
        item = std::move(cell->data);
        cell->sequence.store(pos + buffer_mask_ + 1, std::memory_order_release);
        return true;
    }

private:

    typedef struct __lc_mpmc_queue_cell_t {
        std::atomic<size_t> sequence;
        T                   data;
    } __lc_mpmc_queue_cell_t;

    __lc_cacheline_pad_t    pad0_;
    __lc_mpmc_queue_cell_t *buffer_;
    size_t const            buffer_mask_;
    __lc_cacheline_pad_t    pad1_;
    std::atomic<size_t>     enqueue_index_;
    __lc_cacheline_pad_t    pad2_;
    std::atomic<size_t>     dequeue_index_;
    __lc_cacheline_pad_t    pad3_;
};

enum class LCWriteTaskPriority {
    Critical,        // Logs, metadata, super blocks
    High,            // inode, time, directory entries
    Normal,          // file data
    Low,
    Background,      // background fllushes, prewrites, etc.
    NUM_PRIORITIES,  // Number of priorities defined
};

enum class LCReadTaskPriority {
    Critical,        // metadata, directory traversal, opening file headers
    High,            // user requests, latency-sensitive
    Normal,          // sequential reads, page loading, etc.
    Low,
    Background,      // preread, prefetch, scan, etc.
    NUM_PRIORITIES,  // Number of priorities defined
};

template <typename PriorityType>
struct LCPriorityTraits;

template <>
struct LCPriorityTraits<LCWriteTaskPriority> {
    static LC_CONSTEXPR size_t
    get_priority_queue_size(LCWriteTaskPriority pri) {
        switch (pri) {
            case LCWriteTaskPriority::Critical   : return 64;
            case LCWriteTaskPriority::High       : return 128;
            case LCWriteTaskPriority::Normal     : return 256;
            case LCWriteTaskPriority::Low        : return 512;
            case LCWriteTaskPriority::Background : return 1024;
            default                              : return 0;  // Invalid priority
        }
    }
};

template <>
struct LCPriorityTraits<LCReadTaskPriority> {
    static LC_CONSTEXPR size_t get_priority_queue_size(LCReadTaskPriority pri) {
        switch (pri) {
            case LCReadTaskPriority::Critical   : return 64;
            case LCReadTaskPriority::High       : return 128;
            case LCReadTaskPriority::Normal     : return 256;
            case LCReadTaskPriority::Low        : return 512;
            case LCReadTaskPriority::Background : return 1024;
            default                             : return 0;  // Invalid priority
        }
    }
};

template <class T, class PriorityType>
class LCMPMCMultiPriorityQueue {
    static_assert(
        std::is_same<PriorityType, LCWriteTaskPriority>::value ||
            std::is_same<PriorityType, LCReadTaskPriority>::value,
        "Invalid priority type, must be either LCWriteTaskPriority or LCReadTaskPriority");
public:

    LCMPMCMultiPriorityQueue() {
        size_t num_priorities =
            static_cast<size_t>(PriorityType::NUM_PRIORITIES);
        try {
            queues_ = lc_construct_array_indexed<LCMPMCQueue<T>>(num_priorities,
                                                                 [](size_t i) {
                return LCPriorityTraits<PriorityType>::get_priority_queue_size(
                    static_cast<PriorityType>(i));
            });
            if (!queues_) {
                throw LCBadAllocError(
                    "Failed to allocate multi-priority queue");
            }
        } catch (const LCBadAllocError &e) {
            // print error and panic
            lc_fatal_exception(e);
        }
        size_.store(0, std::memory_order_relaxed);
    }

    ~LCMPMCMultiPriorityQueue() {
        lc_destroy_array(queues_,
                         static_cast<size_t>(PriorityType::NUM_PRIORITIES));
    }

    LCMPMCMultiPriorityQueue(const LCMPMCMultiPriorityQueue &) = delete;
    LCMPMCMultiPriorityQueue &operator=(const LCMPMCMultiPriorityQueue &) =
        delete;
    LCMPMCMultiPriorityQueue(LCMPMCMultiPriorityQueue &&)            = delete;
    LCMPMCMultiPriorityQueue &operator=(LCMPMCMultiPriorityQueue &&) = delete;

    template <typename P            = PriorityType,
              std::enable_if_t<std::is_same_v<P, LCWriteTaskPriority> ||
                                   std::is_same_v<P, LCReadTaskPriority>,
                               int> = 0>
    bool enqueue(const T &item, P priority) {
        size_t index = static_cast<size_t>(priority);
        LC_ASSERT(index < static_cast<size_t>(PriorityType::NUM_PRIORITIES),
                  "Invalid priority index");
        if (queues_[index].enqueue(item)) {
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;  // Queue is full
    }

    template <typename P            = PriorityType,
              std::enable_if_t<std::is_same_v<P, LCWriteTaskPriority> ||
                                   std::is_same_v<P, LCReadTaskPriority>,
                               int> = 0>
    bool enqueue(T &&item, P priority) {
        size_t index = static_cast<size_t>(priority);
        LC_ASSERT(index < static_cast<size_t>(PriorityType::NUM_PRIORITIES),
                  "Invalid priority index");
        if (queues_[index].enqueue(std::move(item))) {
            size_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;  // Queue is full
    }

    template <typename P            = PriorityType,
              std::enable_if_t<std::is_same_v<P, LCWriteTaskPriority> ||
                                   std::is_same_v<P, LCReadTaskPriority>,
                               int> = 0>
    bool dequeue(T &item, P priority) {
        size_t index = static_cast<size_t>(priority);
        LC_ASSERT(index < static_cast<size_t>(PriorityType::NUM_PRIORITIES),
                  "Invalid priority index");
        if (size_.load(std::memory_order_relaxed) == 0) {
            return false;  // Queue is empty
        }
        if (queues_[index].dequeue(item)) {
            size_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
        return false;  // Queue is empty or dequeue failed
    }

    bool is_empty() const {
        return size_.load(std::memory_order_relaxed) == 0;
    }

private:
    LCMPMCQueue<T>     *queues_;
    std::atomic<size_t> size_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_MPMC_QUEUE_H
