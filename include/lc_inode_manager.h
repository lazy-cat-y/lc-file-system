

#ifndef LC_INODE_MANAGER_H
#define LC_INODE_MANAGER_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include "lc_block_manager.h"
#include "lc_configs.h"
#include "lc_inode.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

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
            (inode_cache_info_.inode_count) / 8 +
            ((inode_cache_info_.inode_count % 8) ? 1 : 0);
        inode_cache_info_.inode_bitmap_block_count =
            (blockManager_->get_header()->inode_start -
             blockManager_->get_header()->inode_bitmap_start);

        inode_cache_info_.inode_bitmap_start =
            blockManager_->get_header()->inode_bitmap_start;
        inode_cache_info_.inode_bitmap_size = inode_bitmap_size;
        inode_bitmap_                       = new uint8_t[inode_bitmap_size];
    }

    /*
     * alloc_inode():
     * └─ find free bit → mark used → init metadata → write_inode() → return ino
     * read inode bitmap block - find frist free bit - set it to 1
     * write inode bitmap block - write inode to the block
     * return inode number
     */
    uint32_t alloc_inode(uint16_t mode);
    /*
    * free_inode():
    * └─ load_inode() → decrement or check link count →
       ├─ if >0: update inode metadata
       └─ if 0: truncate_inode() → free blocks → clear metadata/bitmap
     → write_inode()
    */
    void free_inode(uint32_t ino);

    LCInode load_inode(uint32_t ino);
    void    write_inode(uint32_t ino, const LCInode &inode);

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

    uint32_t find_free_inode();

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

    // private
    // const LCBlockManager &blockManager_;
    std::shared_ptr<LCBlockManager> blockManager_;
    uint8_t                        *inode_bitmap_;

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
