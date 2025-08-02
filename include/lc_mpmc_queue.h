

#ifndef LC_MPMC_QUEUE_H
#define LC_MPMC_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "lc_configs.h"
#include "lc_memory.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

#define __LC_CACHE_LINE_SIZE 64
typedef char __lc_cacheline_pad_t[__LC_CACHE_LINE_SIZE];

template <typename T> class LCMPMCQueue {
public:

    LCMPMCQueue(size_t buffer_size) : buffer_mask_(buffer_size - 1) {
        LC_ASSERT(buffer_size >= 2 && (buffer_size & (buffer_size - 1)) == 0,
                  "Buffer size must be a power of two and at least 2.");
        buffer_ = lc_alloc_array<T>(buffer_size);
        for (size_t i = 0; i < buffer_size; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_index_.store(0, std::memory_order_release);
        dequeue_index_.store(0, std::memory_order_release);
    }

    ~LCMPMCQueue() {
        lc_free_array(buffer_);
    }

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
        item = cell->data;
        cell->sequence.store(pos + buffer_mask_ + 1, std::memory_order_release);
        return true;
    }

private:

    typedef struct __lc_mpmc_queue_cell_t {
        std::atomic<size_t> sequence;
        T                   data;
    } __lc_mpmc_queue_cell_t;

    __lc_cacheline_pad_t pad0_;
    __lc_mpmc_queue_cell_t const * volatile buffer_;
    size_t const         buffer_mask_;
    __lc_cacheline_pad_t pad1_;
    std::atomic<size_t>  enqueue_index_;
    __lc_cacheline_pad_t pad2_;
    std::atomic<size_t>  dequeue_index_;
    __lc_cacheline_pad_t pad3_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_MPMC_QUEUE_H
