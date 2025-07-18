#ifndef LC_BLOCK_DEVICE_H
#define LC_BLOCK_DEVICE_H

#include <sys/stat.h>

#include <cstdint>
#include <fstream>
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

class LCBlockDevice {
public:

    LCBlockDevice() = delete;

    LCBlockDevice(const LCBlockDevice &)            = delete;
    LCBlockDevice(LCBlockDevice &&)                 = delete;
    LCBlockDevice &operator=(const LCBlockDevice &) = delete;
    LCBlockDevice &operator=(LCBlockDevice &&)      = delete;

    LC_EXPLICIT LCBlockDevice(const std::string &img_path) {
        LC_ASSERT(!(get_file_size(img_path) & 0xFFF),
                  "Image size must be a multiple of block size");
        img_file_.open(img_path,
                       std::ios::binary | std::ios::out | std::ios::in);
        if (!img_file_.is_open()) {
            throw ImgOpenError("Failed to open image file: " + img_path);
        }

        LCBlock header_block {};
        img_file_.read(reinterpret_cast<char *>(block_as(&header_block)),
                       DEFAULT_BLOCK_SIZE);
        LCSuperBlock *header =
            reinterpret_cast<LCSuperBlock *>(header_block.data);
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
    }

    ~LCBlockDevice() {
        if (img_file_.is_open()) {
            img_file_.close();
        }
    }

    LCSuperBlock get_super_block() const {
        return header_;
    }

    void read_block(uint32_t block_id, LCBlock &block) const {
        LC_ASSERT(block_id != 0,
                  "Cannot read from block 0, reserved for superblock");
        LC_ASSERT(block_id < header_.total_blocks,
                  ("Block ID out of range: " + std::to_string(block_id) +
                   ", max: " + std::to_string(header_.total_blocks - 1))
                      .c_str());
        img_file_.seekg(block_id * DEFAULT_BLOCK_SIZE);
        img_file_.read(reinterpret_cast<char *>(block_as(&block)),
                       DEFAULT_BLOCK_SIZE);
    }

    void write_block(uint32_t block_id, const LCBlock &block) {
        LC_ASSERT(block_id != 0,
                  "Cannot write to block 0, reserved for superblock");
        LC_ASSERT(block_id < header_.total_blocks,
                  ("Block ID out of range: " + std::to_string(block_id) +
                   ", max: " + std::to_string(header_.total_blocks - 1))
                      .c_str());
        img_file_.seekp(block_id * DEFAULT_BLOCK_SIZE);
        img_file_.write(reinterpret_cast<const char *>(block_as_const(&block)),
                        DEFAULT_BLOCK_SIZE);
    }


private:
    mutable std::fstream img_file_;
    LCSuperBlock         header_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_DEVICE_H
