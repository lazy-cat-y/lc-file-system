#ifndef LC_INODE_BUFFER_H
#define LC_INODE_BUFFER_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "lc_block_buffer.h"
#include "lc_configs.h"
#include "lc_inode.h"

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

    ~LCInodeBufferPool();

    LCInodeBufferPool(LCBlockBufferPool *block_buffer_pool, uint32_t pool_size,
                      uint32_t frame_interval_ms) :
        block_buffer_pool_(block_buffer_pool),
        frames_(new LCInodeBufferPoolFrame[pool_size]),
        pool_size_(pool_size),
        frame_interval_ms_(frame_interval_ms),
        clock_hand_(0) {}

private:

    void free_resources() {
        if (frames_) {
            delete[] frames_;
            frames_ = nullptr;
        }
        if (frame_locks_) {
            delete[] frame_locks_;
            frame_locks_ = nullptr;
        }
    }

    // We can not hold the ownership of the block buffer pool here,
    // just point to it.
    LCBlockBufferPool                     *block_buffer_pool_;
    LCInodeBufferPoolFrame                *frames_;
    uint32_t                               pool_size_;
    uint32_t                               frame_interval_ms_;
    std::unordered_map<uint32_t, uint32_t> frame_map_;

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
