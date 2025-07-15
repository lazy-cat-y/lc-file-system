

#ifndef LC_INODE_MANAGER_H
#define LC_INODE_MANAGER_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include "lc_block.h"
#include "lc_block_manager.h"
#include "lc_configs.h"
#include "lc_inode.h"
#include "lc_utils.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

// TODO: change read and write methods to use LCBlockBufferPool
// TODO: add caching mechanism for frequently accessed inodes

class LCInodeManager {
#if defined(DEBUG) || defined(_DEBUG)
    static_assert(sizeof(LCInode) == LC_INODE_SIZE, "LCInode size mismatch");
    friend class LCInodeManagerTestAccess;
#endif
public:

    LCInodeManager() = delete;

    ~LCInodeManager() {
        if (inode_bitmap_) {
            delete[] inode_bitmap_;
        }
    };

    LCInodeManager(const LCInodeManager &)            = delete;
    LCInodeManager &operator=(const LCInodeManager &) = delete;
    LCInodeManager(LCInodeManager &&)                 = delete;
    LCInodeManager &operator=(LCInodeManager &&)      = delete;

    LCInodeManager(std::shared_ptr<LCBlockManager> blockManager) :
        blockManager_ {std::move(blockManager)},
        inode_bitmap_ {nullptr},
        inode_cache_info_ {} {
        read_inode_bitmap();

        inode_cache_info_.inode_count =
            blockManager_->get_header()->inode_count;
        uint32_t inode_bitmap_size =
            ceil_divide_int32_t(inode_cache_info_.inode_count, 8);
        // (inode_cache_info_.inode_count) / 8 +
        // ((inode_cache_info_.inode_count % 8) ? 1 : 0);
        inode_cache_info_.inode_bitmap_block_count =
            (blockManager_->get_header()->inode_start -
             blockManager_->get_header()->inode_bitmap_start);

        inode_cache_info_.inode_bitmap_start =
            blockManager_->get_header()->inode_bitmap_start;
        inode_cache_info_.inode_bitmap_size = inode_bitmap_size;
        inode_bitmap_                       = new uint8_t[inode_bitmap_size];
        init_last_hint();
    }

    /*
     * alloc_inode():
     * └─ find free bit → mark used → init metadata → write_inode() → return ino
     * read inode bitmap block - find frist free bit - set it to 1
     * write inode bitmap block - write inode to the block
     * return inode number
     */
    uint32_t alloc_inode(uint16_t mode, uint32_t uid, uint32_t gid) {
        uint32_t ino = find_free_inode();
        set_inode_bitmap_bit(ino);
        LCInode inode {};
        // TODO: Add ctime
        inode.mode       = mode;
        inode.uid        = uid;
        inode.gid        = gid;
        inode.size       = 0;
        inode.atime      = 0;
        inode.mtime      = 0;
        inode.ctime      = 0;
        inode.dtime      = 0;
        inode.link_count = 1;
        inode.blocks     = 0;
        inode.generation = 1;
        inode.cr_time    = 0;
        memset(inode.block_ptr, 0, sizeof(inode.block_ptr));

        write_inode(ino, inode);
        return ino;
    }

    /*
    * free_inode():
    * └─ load_inode() → decrement or check link count →
       ├─ if >0: update inode metadata
       └─ if 0: truncate_inode() → free blocks → clear metadata/bitmap
     → write_inode()
    */
    void free_inode(uint32_t ino) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        LCInode inode = load_inode(ino);
        LC_ASSERT(inode.link_count >= 0, "Inode link count is negative");
        if (inode.link_count > 1) {
            --inode.link_count;
            write_inode(ino, inode);
        } else {
            truncate_inode(inode, 0);  // Free blocks and clear metadata
            clear_inode_bitmap_bit(ino);
            write_inode(ino, inode);   // Write updated inode
        }
    }

    /*
     * load_inode():
     * └─ read inode from the block manager
     * TODO: Maybe add caching mechanism for frequently accessed inodes
     *       to avoid reading from disk every time
     *       (e.g., LRU cache or similar), a buffter pool, also we need a
     *       mapping inode numbers to their cached locations, a key-value
     *       store-like structure. And the buffer pool should be stored block
     *       by block, not inode by inode.
     *       Do we need a new class for this?
     */
    LCInode load_inode(uint32_t ino) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        LCInode  inode {};
        uint32_t block_index = ino / LC_INODES_PRE_BLOCK;
        uint32_t offset      = (ino % LC_INODES_PRE_BLOCK) * LC_INODE_SIZE;
        LCBlock  inode_block {};
        blockManager_->read_block(block_index, inode_block);
        memcpy(&inode, block_as(&inode_block) + offset, sizeof(LCInode));
        return inode;
    }

    /*
     * write_inode():
     * └─ load_inode() → update inode metadata → write to the block manager
     * If we have a caching mechanism, we would update the cache, but when
     * should we write to disk? Every time we update the inode?
     */
    void write_inode(uint32_t ino, const LCInode &inode) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        uint32_t block_index = ino / LC_INODES_PRE_BLOCK;
        uint32_t offset      = (ino % LC_INODES_PRE_BLOCK) * LC_INODE_SIZE;
        LCBlock  inode_block {};
        blockManager_->read_block(block_index, inode_block);
        memcpy(block_as(&inode_block) + offset, &inode, sizeof(LCInode));
        blockManager_->write_block(block_index, inode_block);
    }

    uint32_t get_block_id(LCInode &inode, uint32_t file_blk, bool allocate);

    void read_file_block(LCInode &inode, uint32_t file_blk, char *buf);
    void write_file_block(LCInode &inode, uint32_t file_blk, const char *buf);

    void truncate_inode(LCInode &inode, uint64_t new_size);

    void update_atime(LCInode &inode);
    void update_mtime(LCInode &inode);
    void update_ctime(LCInode &inode);

    Stat stat_inode(const LCInode &inode);

    uint32_t lookup(const LCInode &dir_inode, const std::string &name);
    void     add_dir_entry(LCInode &dir_inode, const std::string &name,
                           uint32_t child_ino);
    void     remove_dir_entry(LCInode &dir_inode, const std::string &name);

private:

    uint32_t find_free_inode() {
        for (uint32_t i = inode_last_hint_; i < inode_cache_info_.inode_count;
             ++i) {
            if (!test_inode_bitmap_bit(i)) {
                inode_last_hint_ = i + 1;
                return i;
            }
        }
        for (uint32_t i = 0; i < inode_last_hint_; ++i) {
            if (!test_inode_bitmap_bit(i)) {
                inode_last_hint_ = i + 1;
                return i;
            }
        }
        // Or throw an exception or handle error
        LC_ASSERT(false, "No free inodes available");
        return 0;  // Unreachable, but avoids compiler warning
    }

    void read_inode_bitmap() {
        LCBlock inode_bitmap_block {};
        for (uint32_t i = 0; i < inode_cache_info_.inode_bitmap_block_count;
             ++i) {
            blockManager_->read_block(inode_cache_info_.inode_bitmap_start + i,
                                      inode_bitmap_block);
            uint32_t offset = i * DEFAULT_BLOCK_SIZE;
            uint32_t size   = std::min<uint32_t>(
                DEFAULT_BLOCK_SIZE,
                inode_cache_info_.inode_bitmap_size - offset);
            memcpy(inode_bitmap_ + offset, block_as(&inode_bitmap_block), size);
        }
    }

    void write_inode_bitmap() {
        LCBlock inode_bitmap_block {};
        for (uint32_t i = 0; i < inode_cache_info_.inode_bitmap_block_count;
             ++i) {
            uint32_t offset = i * DEFAULT_BLOCK_SIZE;
            uint32_t size   = std::min<uint32_t>(
                DEFAULT_BLOCK_SIZE,
                inode_cache_info_.inode_bitmap_size - offset);
            memcpy(block_as(&inode_bitmap_block), inode_bitmap_ + offset, size);
            blockManager_->write_block(inode_cache_info_.inode_bitmap_start + i,
                                       inode_bitmap_block);
        }
    }

    void set_inode_bitmap_bit(uint32_t ino) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        uint32_t byte_index = ino / 8;
        uint32_t bit_index  = ino % 8;
        LC_ASSERT(byte_index < inode_cache_info_.inode_bitmap_size,
                  "Inode bitmap index out of range");
        inode_bitmap_[byte_index] |= (1 << bit_index);
    }

    void clear_inode_bitmap_bit(uint32_t ino) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        uint32_t byte_index = ino / 8;
        uint32_t bit_index  = ino % 8;
        LC_ASSERT(byte_index < inode_cache_info_.inode_bitmap_size,
                  "Inode bitmap index out of range");
        inode_bitmap_[byte_index] &= ~(1 << bit_index);
    }

    bool test_inode_bitmap_bit(uint32_t ino) const {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        uint32_t byte_index = ino / 8;
        uint32_t bit_index  = ino % 8;
        LC_ASSERT(byte_index < inode_cache_info_.inode_bitmap_size,
                  "Inode bitmap index out of range");
        return (inode_bitmap_[byte_index] & (1 << bit_index)) != 0;
    }

    void init_last_hint() {
        for (uint32_t i = 0; i < inode_cache_info_.inode_count; ++i) {
            if (!test_inode_bitmap_bit(i)) {
                inode_last_hint_ = i;
                return;
            }
        }
        inode_last_hint_ = 0;  // No inodes allocated, start from 0
    }

    // private
    // const LCBlockManager &blockManager_;
    std::shared_ptr<LCBlockManager> blockManager_;
    uint8_t                        *inode_bitmap_;
    uint32_t                        inode_last_hint_;

    struct {
        uint32_t inode_count;
        uint32_t inode_bitmap_start;
        uint32_t inode_bitmap_block_count;
        uint32_t inode_bitmap_size;
    } inode_cache_info_;
};

#if defined(DEBUG) || defined(_DEBUG)
class LCInodeManagerTestAccess {
public:

    static uint8_t *get_inode_bitmap(LCInodeManager &manager) {
        return manager.inode_bitmap_;
    }

    static void read_inode_bitmap(LCInodeManager &manager) {
        manager.read_inode_bitmap();
    }

    static void write_inode_bitmap(LCInodeManager &manager) {
        manager.write_inode_bitmap();
    }

    static void set_inode_bitmap_bit(LCInodeManager &manager, uint32_t ino) {
        manager.set_inode_bitmap_bit(ino);
    }

    static void clear_inode_bitmap_bit(LCInodeManager &manager, uint32_t ino) {
        manager.clear_inode_bitmap_bit(ino);
    }

    static bool test_inode_bitmap_bit(const LCInodeManager &manager,
                                      uint32_t              ino) {
        return manager.test_inode_bitmap_bit(ino);
    }
};
#endif  // DEBUG || _DEBUG

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_INODE_MANAGER_H
