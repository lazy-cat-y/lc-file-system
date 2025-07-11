#ifndef LC_BLOCK_MANAGER_H
#define LC_BLOCK_MANAGER_H

#include <sys/stat.h>

#include <cstdint>
#include <fstream>
#include <ios>
#include <string>

#include "lc_block.h"
#include "lc_configs.h"
#include "lc_exception.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

static uint64_t get_file_size(const std::string &file_path) {
    struct stat file_stat;
    if (stat(file_path.c_str(), &file_stat) != 0) {
        throw FileNotFoundError(file_path);
    }
    return file_stat.st_size;
}

class LCBlockManager {
public:
    LCBlockManager() = default;

    ~LCBlockManager() {
        if (img_file_.is_open()) {
            img_file_.close();
        }
    }

    LCBlockManager(const std::string &img_path) : img_path_ {img_path} {
        uint64_t total_size_bytes = get_file_size(img_path);

        img_file_.open(img_path,
                       std::ios::binary | std::ios::out | std::ios::in);

        if (!img_file_.is_open()) {
            throw ImgOpenError("Failed to open image file: " + img_path);
        }

        LCBlock header_block {};
        img_file_.read(reinterpret_cast<char *>(block_as(&header_block)),
                       DEFAULT_BLOCK_SIZE);
        LCBlockHeader *header =
            reinterpret_cast<LCBlockHeader *>(header_block.data);
        if (header->magic != BLOCK_MAGIC_NUMBER) {
            throw ImgMagicError("Invalid image magic number: " + img_path);
        }
        header_ = *header;

        if (header_.block_size != DEFAULT_BLOCK_SIZE) {
            throw UnsupportedBlockSizeError(
                "Unsupported block size in image: " + img_path + ". Expected " +
                std::to_string(DEFAULT_BLOCK_SIZE) + ", got " +
                std::to_string(header->block_size));
        }
        if (header_.total_blocks * DEFAULT_BLOCK_SIZE != total_size_bytes) {
            throw ImgSizeMismatchError("Image size does not match header: " +
                                       img_path);
        }
    }

    LCBlockManager(const LCBlockManager &)            = delete;
    LCBlockManager &operator=(const LCBlockManager &) = delete;
    LCBlockManager(LCBlockManager &&)                 = delete;
    LCBlockManager &operator=(LCBlockManager &&)      = delete;

    static void format(const std::string &img_path,
                       const uint64_t     total_size_bytes) {
        // Ensure the total size is a multiple of the block size
        LC_ASSERT(!(total_size_bytes & 0xFFF),
                  "Total size must be a multiple of block size");

        LC_CONSTEXPR uint32_t bytes_per_inode = 16 * 1024;  // 16 KiB per inode

        uint32_t total_blocks = total_size_bytes / DEFAULT_BLOCK_SIZE;
        uint32_t inode_count  = total_size_bytes / bytes_per_inode;
        uint32_t inode_block_count =
            (inode_count * sizeof(LCBlockHeader) + DEFAULT_BLOCK_SIZE - 1) /
            DEFAULT_BLOCK_SIZE;

        uint32_t block_bitmap_size = (total_blocks) / 8 + 
                                      ((total_blocks % 8) ? 1 : 0);
        uint32_t block_bitmap_block_count =
                        (block_bitmap_size) / DEFAULT_BLOCK_SIZE +
                        ((block_bitmap_size % DEFAULT_BLOCK_SIZE) ? 1 : 0);
        uint32_t inode_bitmap_size = (inode_count) / 8 +
                                     ((inode_count % 8) ? 1 : 0);
        uint32_t inode_bitmap_block_count =
            (inode_bitmap_size) / DEFAULT_BLOCK_SIZE +
            ((inode_bitmap_size % DEFAULT_BLOCK_SIZE) ? 1 : 0);

        LCBlockHeader header {};
        header.total_blocks       = total_blocks;
        header.inode_count        = inode_count;
        header.block_bitmap_start = 1;  // Usually starts at block 1
        header.inode_bitmap_start = header.block_bitmap_start + 
                                    block_bitmap_block_count;
        header.inode_start        = header.inode_bitmap_start + inode_bitmap_block_count;
        header.inode_block_count  = inode_block_count;
        header.data_start         = header.inode_start + inode_block_count;

        LCBlock header_block {};
        block_clear(&header_block);
        block_write(&header_block, &header, sizeof(header));

        std::ofstream img_file(img_path, std::ios::binary);
        if (!img_file) {
            throw ImgCreationError("Failed to create image file: " + img_path);
        }

        img_file.write(reinterpret_cast<char *>(block_as(&header_block)),
                       DEFAULT_BLOCK_SIZE);
        LCBlock ZeroBlock {};
        block_clear(&ZeroBlock);
        for (uint32_t i = 1; i < total_blocks; ++i) {
            img_file.write(reinterpret_cast<char *>(block_as(&ZeroBlock)),
                           DEFAULT_BLOCK_SIZE);
        }
        img_file.close();
    }

    void read_block(uint32_t block_id, LCBlock &block) const {
        LC_ASSERT(block_id < header_.total_blocks,
                  ("Block ID out of range: " + std::to_string(block_id) +
                   ", max: " + std::to_string(header_.total_blocks - 1))
                      .c_str());

        img_file_.seekg(block_id * DEFAULT_BLOCK_SIZE, std::ios::beg);
        img_file_.read(reinterpret_cast<char *>(block_as(&block)),
                       DEFAULT_BLOCK_SIZE);
    }

    void write_block(uint32_t block_id, const LCBlock &block) {
        LC_ASSERT(block_id < header_.total_blocks,
                  ("Block ID out of range: " + std::to_string(block_id) +
                   ", max: " + std::to_string(header_.total_blocks - 1))
                      .c_str());

        img_file_.seekp(block_id * DEFAULT_BLOCK_SIZE, std::ios::beg);
        img_file_.write(reinterpret_cast<const char *>(block_as_const(&block)),
                        DEFAULT_BLOCK_SIZE);
    }

    const LCBlockHeader *get_header() const {
        return &header_;
    }

private:
    std::string   img_path_;
    LCBlockHeader header_;
    // This one should be thread local
    mutable std::fstream img_file_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_MANAGER_H
