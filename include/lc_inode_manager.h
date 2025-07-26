

#ifndef LC_INODE_MANAGER_H
#define LC_INODE_MANAGER_H

#include <sys/types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "lc_block.h"
#include "lc_block_manager.h"
#include "lc_configs.h"
#include "lc_exception.h"
#include "lc_inode.h"
#include "lc_inode_buffer.h"
#include "lc_memory.h"
#include "lc_utils.h"

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
        lc_free_array(inode_bitmap_);
    };

    LCInodeManager(const LCInodeManager &)            = delete;
    LCInodeManager &operator=(const LCInodeManager &) = delete;
    LCInodeManager(LCInodeManager &&)                 = delete;
    LCInodeManager &operator=(LCInodeManager &&)      = delete;

    LC_EXPLICIT LCInodeManager(const LCSuperBlock &block_header,
                               LCInodeBufferPool  *inode_buffer_pool,
                               LCBlockManager     *block_manager) :
        inode_buffer_pool_ {inode_buffer_pool},
        block_manager_ {block_manager},
        inode_cache_info_ {} {
        lc_memset(&inode_cache_info_, 0, sizeof(inode_cache_info_));

        inode_cache_info_.inode_count =
            block_header.inode_count;  // Total number of inodes
        inode_cache_info_.inode_bitmap_size =
            lc_ceil_divide_int32_t(inode_cache_info_.inode_count, 8);
        inode_cache_info_.inode_bitmap_block_count =
            lc_ceil_divide_int32_t(inode_cache_info_.inode_bitmap_size,
                                   DEFAULT_BLOCK_SIZE);
        inode_cache_info_.inode_bitmap_start = block_header.inode_bitmap_start;
        inode_cache_info_.inode_bitmap_size =
            inode_cache_info_.inode_bitmap_size;

        inode_bitmap_ =
            lc_alloc_array<uint8_t>(inode_cache_info_.inode_bitmap_size);

        read_inode_bitmap();
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
        LC_ASSERT(test_inode_bitmap_bit(ino), "Inode is not allocated");
        {
            LCInodeFrameGuard inode_guard =
                inode_buffer_pool_->access_frame_lock(ino);
            auto &frame = inode_guard.frame;
            if (frame->inode.link_count > 1) {
                --frame->inode.link_count;
                inode_guard.mark_dirty();
            } else {
                truncate_inode(&frame->inode);
                // Clear metadata, actually we do not need to clear metadata,
                // next time when we alloc it will be reinitialized to default
                // value
                clear_inode_bitmap_bit(ino);
            }
            inode_guard.mark_dirty();
        }
    }

    LCInode load_inode(uint32_t ino) {
        LC_ASSERT(ino < inode_cache_info_.inode_count,
                  "Inode number out of range");
        LCInode inode {};
        inode_buffer_pool_->read_inode(ino, inode);
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
        inode_buffer_pool_->write_inode(ino, inode);
        // blockManager_->read_block(block_index, inode_block);
        // memcpy(block_as(&inode_block) + offset, &inode, sizeof(LCInode));
        // blockManager_->write_block(block_index, inode_block);
    }

    // FIXME: Change inode to inode_id or write a inode_id version
    uint32_t get_block_id(LCInode &inode, uint32_t file_blk, bool allocate) {
        LC_ASSERT(
            file_blk <
                LC_PTRS_PRE_BLOCK + LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK +
                    LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK,
            "Invalid file block");

        if (file_blk < LC_DIRECT_PTRS) {
            if (inode.block_ptr[file_blk] == LC_BLOCK_ILLEGAL_ID) {
                if (!allocate) {
                    return LC_BLOCK_ILLEGAL_ID;
                }
                inode.block_ptr[file_blk] = block_manager_->alloc_block();
                inode.blocks++;
            }
            return inode.block_ptr[file_blk];
        }

        // [12, 15)
        for (uint32_t i = LC_DIRECT_PTRS; i < LC_BLOCK_PTRS_SIZE; ++i) {
            file_blk -= LC_PTRS_PRE_BLOCK;
            if (file_blk < LC_PTRS_PRE_BLOCK) {
                return get_indirect_block_id(inode.block_ptr[i],
                                             file_blk,
                                             i - LC_DIRECT_PTRS + 1,
                                             allocate);
            }
        }
        return LC_BLOCK_ILLEGAL_ID;
    }

    // TODO: Implement read file and write file one block per time,
    // return error code if block is not read
    void read_file_block(uint32_t inode_id, uint32_t file_blk, char *buf) {
        LC_ASSERT(
            file_blk <
                LC_PTRS_PRE_BLOCK + LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK +
                    LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK,
            "Invalid file block");
        {
            LCInodeFrameGuard inode_guard =
                inode_buffer_pool_->access_frame_lock(inode_id);
            auto    &frame    = inode_guard.frame;
            uint32_t block_id = get_block_id(frame->inode, file_blk, false);
            // FUTURE: maybe we should return an error code here
            if (block_id == LC_BLOCK_ILLEGAL_ID) {
                lc_memset(buf, 0, DEFAULT_BLOCK_SIZE);
                return;  // Block not allocated
            }
            block_manager_->read_block(block_id, buf, DEFAULT_BLOCK_SIZE);
        }
    }

    void write_file_block(uint32_t inode_id, uint32_t file_blk,
                          const char *buf) {
        LC_ASSERT(
            file_blk <
                LC_PTRS_PRE_BLOCK + LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK +
                    LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK,
            "Invalid file block");
        {
            LCInodeFrameGuard inode_guard =
                inode_buffer_pool_->access_frame_lock(inode_id);
            auto    &frame    = inode_guard.frame;
            uint32_t block_id = get_block_id(frame->inode, file_blk, true);
            // FUTURE: maybe we should return an error code here
            if (block_id == LC_BLOCK_ILLEGAL_ID) {
                LC_ASSERT(false, "Block not allocated, this should not happen");
                return;  // Block not allocated
            }
            block_manager_->write_block(block_id, buf, DEFAULT_BLOCK_SIZE);
            frame->inode.blocks++;
            inode_guard.mark_dirty();
        }
    }

    void truncate_inode(LCInode *inode) {
        for (uint32_t i = 0; i < LC_DIRECT_PTRS; ++i) {
            if (inode->block_ptr[i] != LC_BLOCK_ILLEGAL_ID) {
                block_manager_->free_block(inode->block_ptr[i]);
                inode->block_ptr[i] = LC_BLOCK_ILLEGAL_ID;
            }
        }

        // three levels of indirection
        // 12 13 14
        for (uint32_t i = LC_DIRECT_PTRS; i < LC_BLOCK_PTRS_SIZE; ++i) {
            if (inode->block_ptr[i] != LC_BLOCK_ILLEGAL_ID) {
                free_indirect_block(inode->block_ptr[i],
                                    i - LC_DIRECT_PTRS + 1);
                inode->block_ptr[i] = LC_BLOCK_ILLEGAL_ID;
            }
        }
    }

    // void update_atime(LCInode &inode);
    // void update_mtime(LCInode &inode);
    // void update_ctime(LCInode &inode);

    Stat stat_inode(const uint32_t inode_id) {
        LC_ASSERT(inode_id < inode_cache_info_.inode_count,
                  "Inode ID out of range");
        LCInode inode {};
        inode_buffer_pool_->read_inode(inode_id, inode);
        Stat stat {};
        stat.ino    = inode_id;
        stat.mode   = inode.mode;
        stat.uid    = inode.uid;
        stat.gid    = inode.gid;
        stat.size   = inode.size;
        stat.blocks = inode.blocks;
        // stat.st_atime    = inode.atime;
        // stat.st_mtime    = inode.mtime;
        // stat.st_ctime    = inode.ctime;
        return stat;
    }

    // Return Error code
    uint32_t create_directory(uint32_t parent_ino, const std::string &name,
                              uint32_t uid, uint32_t gid) {
        uint32_t name_len = name.length();
        LC_ASSERT(name_len >= 0, "Name length is negative");
        if (name_len > LC_FILE_NAME_MAX_LEN || name_len == 0) {
            throw LCInvalidFileLenError(name_len);
        }

        uint32_t ino = alloc_inode(LC_S_IFDIR, uid, gid);
        {
            LCInodeFrameGuard parent_inode_guard =
                inode_buffer_pool_->access_frame_lock(parent_ino);
            if (lookup(parent_inode_guard.frame->inode, name) !=
                LC_BLOCK_ILLEGAL_ID) {
                throw LCFileExistsError(name);
            }

            LCInodeFrameGuard current_inode_guard =
                inode_buffer_pool_->access_frame_lock(ino);
            add_dir_entry(current_inode_guard.frame->inode,
                          LC_CURRUNT_DIRECTORY,
                          ino);
            add_dir_entry(current_inode_guard.frame->inode,
                          LC_PARENT_DIRECTORY,
                          parent_ino);
            current_inode_guard.mark_dirty();
            add_dir_entry(parent_inode_guard.frame->inode, name, ino);
            parent_inode_guard.mark_dirty();
        }
        return ino;
    }

    void add_dir_entry(uint32_t cur_inode_id, const std::string &name,
                       uint32_t child_inode_id) {
        LCInodeFrameGuard inode_guard =
            inode_buffer_pool_->access_frame_lock(cur_inode_id);
        add_dir_entry(inode_guard.frame->inode, name, child_inode_id);
    }

    void remove_dir_entry(uint32_t cur_inode_id, const std::string &name) {
        LCInodeFrameGuard inode_guard =
            inode_buffer_pool_->access_frame_lock(cur_inode_id);
        remove_dir_entry(inode_guard.frame->inode, name);
    }


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

    void free_indirect_block(uint32_t block_id, uint32_t level) {
        if (level == 0 || block_id == LC_BLOCK_ILLEGAL_ID) {
            return;
        }
        LCBlock ptr_block {};
        block_manager_->read_block(block_id, ptr_block);
        uint32_t *ptrs = reinterpret_cast<uint32_t *>(block_as(&ptr_block));
        for (uint32_t i = 0; i < LC_PTRS_PRE_BLOCK; ++i) {
            if (ptrs[i] == LC_BLOCK_ILLEGAL_ID) {
                continue;
            }

            if (level == 1) {
                block_manager_->free_block(ptrs[i]);
            } else {
                free_indirect_block(ptrs[i], level - 1);
            }
        }

        block_manager_->free_block(block_id);
    }

    void read_inode_bitmap() {
        LCBlock  inode_bitmap_block {};
        uint32_t offset      = 0;
        uint32_t size        = 0;
        uint32_t block_index = 0;
        for (uint32_t i = 0; i < inode_cache_info_.inode_bitmap_block_count;
             ++i) {
            block_index = inode_cache_info_.inode_bitmap_start + i;
            block_manager_->read_block(block_index, inode_bitmap_block);

            offset = i * DEFAULT_BLOCK_SIZE;
            size   = std::min<uint32_t>(
                DEFAULT_BLOCK_SIZE,
                inode_cache_info_.inode_bitmap_size - offset);
            lc_memcpy(inode_bitmap_ + offset,
                      block_as(&inode_bitmap_block),
                      size);
        }
    }

    void write_inode_bitmap() {
        LCBlock  inode_bitmap_block {};
        uint32_t offset      = 0;
        uint32_t size        = 0;
        uint32_t block_index = 0;
        for (uint32_t i = 0; i < inode_cache_info_.inode_bitmap_block_count;
             ++i) {
            offset = i * DEFAULT_BLOCK_SIZE;
            size   = std::min<uint32_t>(
                DEFAULT_BLOCK_SIZE,
                inode_cache_info_.inode_bitmap_size - offset);
            lc_memcpy(block_as(&inode_bitmap_block),
                      inode_bitmap_ + offset,
                      size);
            block_index = inode_cache_info_.inode_bitmap_start + i;
            block_manager_->write_block(block_index, inode_bitmap_block);
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

    // Since logically we locked the inode for the block, so we can assume
    // that the ptr block we read is multi-thread safe.
    uint32_t get_indirect_block_id(uint32_t &block_ptr, uint32_t file_blk,
                                   uint32_t level, bool allocate) {
        LC_ASSERT(level > 0 && level <= 3, "Invalid indirect level");
        if (block_ptr == LC_BLOCK_ILLEGAL_ID) {
            if (!allocate) {
                return LC_BLOCK_ILLEGAL_ID;
            }
            block_ptr = block_manager_->alloc_block();
        }
        LCBlock ptr_block {};
        block_manager_->read_block(block_ptr, ptr_block);
        uint32_t *ptrs = lc_uint8_array_to_uint32_array(block_as(&ptr_block));

        uint32_t index = 0;
        switch (level) {
            case 1 : index = file_blk; break;
            case 2 : index = file_blk / LC_PTRS_PRE_BLOCK; break;
            case 3 :
                index = file_blk / (LC_PTRS_PRE_BLOCK * LC_PTRS_PRE_BLOCK);
                break;
            default : LC_ASSERT(false, "Invalid indirect level");
        }

        LC_ASSERT(index < LC_PTRS_PRE_BLOCK, "Invalid indirect block index");

        if (level == 1) {
            if (ptrs[index] == LC_BLOCK_ILLEGAL_ID && allocate) {
                ptrs[index] = block_manager_->alloc_block();
                block_manager_->write_block(block_ptr, ptr_block);
            }
            return ptrs[index];
        } else {
            uint32_t next_block_offset = 0;
            switch (level) {
                case 2 :
                    next_block_offset = file_blk % LC_PTRS_PRE_BLOCK;
                    break;
                case 3 :
                    next_block_offset =
                        (index / LC_PTRS_PRE_BLOCK) % LC_PTRS_PRE_BLOCK;
                    break;
                default : LC_ASSERT(false, "Invalid indirect level");
            }
            return get_indirect_block_id(ptrs[index],
                                         next_block_offset,
                                         level - 1,
                                         allocate);
        }
        return LC_BLOCK_ILLEGAL_ID;  // Unreachable, but avoids compiler
                                     // warning
    }

    uint32_t lookup(const LCInode &dir_inode, const std::string &name) {
        for (uint32_t i = 0; i < dir_inode.blocks; ++i) {
            uint32_t block_id =
                get_block_id(const_cast<LCInode &>(dir_inode), i, false);
            if (block_id == LC_BLOCK_ILLEGAL_ID) {
                continue;
            }

            LCBlock block {};
            block_manager_->read_block(block_id, block);
            LCDirEntry *dir_entries =
                reinterpret_cast<LCDirEntry *>(block_as(&block));
            for (uint32_t j = 0; j < LC_DIRECTORY_ENTRIES_PER_BLOCK; ++j) {
                if (dir_entries[j].inode_id == LC_INODE_ILLEGAL_ID) {
                    continue;  // Skip empty entries
                }
                if (dir_entries[j].name_len == name.length() &&
                    strncmp(dir_entries[j].name,
                            name.c_str(),
                            dir_entries[j].name_len) == 0) {
                    return dir_entries[j].inode_id;  // Found
                }
            }
        }
        return LC_BLOCK_ILLEGAL_ID;  // Not found
    }

    void add_dir_entry(LCInode &dir_inode, const std::string &name,
                       uint32_t child_ino) {
        LC_ASSERT(name.length() < 0, "Name length is negative");
        if (name.length() == 0 || name.length() > LC_FILE_NAME_MAX_LEN) {
            throw LCInvalidFileLenError(name.length());
        }
        for (uint32_t i = 0; i < dir_inode.blocks; ++i) {
            uint32_t block_id = get_block_id(dir_inode, i, false);
            if (block_id == LC_BLOCK_ILLEGAL_ID) {
                continue;  // Block not allocated
            }
            LCBlock block {};
            block_manager_->read_block(block_id, block);
            LCDirEntry *dir_entries =
                reinterpret_cast<LCDirEntry *>(block_as(&block));
            for (uint32_t j = 0; j < LC_DIRECTORY_ENTRIES_PER_BLOCK; ++j) {
                if (dir_entries[j].inode_id != LC_INODE_ILLEGAL_ID) {
                    continue;
                }
                // Found an empty entry
                dir_entries[j].inode_id = child_ino;
                dir_entries[j].name_len = name.length();
                lc_memcpy(dir_entries[j].name, name.c_str(), name.length());
                block_manager_->write_block(block_id, block);
                return;  // Entry added successfully
            }
        }

        uint32_t new_block_id = get_block_id(dir_inode, dir_inode.blocks, true);
        dir_inode.blocks++;
        LCBlock new_block {};
        block_clear(&new_block);
        LCDirEntry *new_dir_entries =
            reinterpret_cast<LCDirEntry *>(block_as(&new_block));
        for (uint32_t i = 0; i < LC_DIRECTORY_ENTRIES_PER_BLOCK; ++i) {
            new_dir_entries[i].inode_id = LC_INODE_ILLEGAL_ID;
        }
        new_dir_entries[0].inode_id = child_ino;
        new_dir_entries[0].name_len = name.length();
        lc_memcpy(new_dir_entries[0].name, name.c_str(), name.length());
        block_manager_->write_block(new_block_id, new_block);
    }

    void remove_dir_entry(LCInode &dir_inode, const std::string &name) {
        for (uint32_t i = 0; i < dir_inode.blocks; ++i) {
            uint32_t block_id = get_block_id(dir_inode, i, false);
            if (block_id == LC_BLOCK_ILLEGAL_ID) {
                continue;
            }
            LCBlock block {};
            block_manager_->read_block(block_id, block);
            LCDirEntry *dir_entries =
                reinterpret_cast<LCDirEntry *>(block_as(&block));
            for (uint32_t j = 0; j < LC_DIRECTORY_ENTRIES_PER_BLOCK &&
                                 dir_entries[j].inode_id != LC_INODE_ILLEGAL_ID;
                 ++j) {
                if (dir_entries[j].name_len == name.length() &&
                    strncmp(dir_entries[j].name,
                            name.c_str(),
                            dir_entries[j].name_len) == 0) {
                    // Found the entry to remove
                    dir_entries[j].inode_id = LC_INODE_ILLEGAL_ID;
                    block_manager_->write_block(block_id, block);
                    return;  // Entry removed successfully
                }
            }
        }
    }

    // private
    // const LCBlockManager &blockManager_;
    LCInodeBufferPool *inode_buffer_pool_;
    LCBlockManager    *block_manager_;
    // TODO: Move this to BitmapCache
    uint8_t *inode_bitmap_;
    uint32_t inode_last_hint_;

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
