#ifndef LC_BLOCK_DEVICE_H
#define LC_BLOCK_DEVICE_H

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
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
    LCBlockDevice()                                 = delete;
    LCBlockDevice(const LCBlockDevice &)            = delete;
    LCBlockDevice &operator=(const LCBlockDevice &) = delete;
    LCBlockDevice(LCBlockDevice &&)                 = delete;
    LCBlockDevice &operator=(LCBlockDevice &&)      = delete;

    explicit LCBlockDevice(const std::string &img_path) {
        LC_ASSERT(!(get_file_size(img_path) & 0xFFF),
                  "Image size must be a multiple of block size");

        fd_ = ::open(img_path.c_str(), O_RDWR | O_DIRECT /*可选*/, 0644);
        if (fd_ == -1) {
            throw ImgOpenError("open failed: " + img_path);
        }

        // 读取 superblock（block 0）
        LCBlock header_block {};
        ssize_t n = ::pread(fd_, header_block.data, DEFAULT_BLOCK_SIZE, 0);
        LC_ASSERT(n == DEFAULT_BLOCK_SIZE, "pread superblock failed");

        auto *header = reinterpret_cast<LCSuperBlock *>(header_block.data);
        if (header->magic != BLOCK_MAGIC_NUMBER) {
            throw ImgMagicError("Invalid magic: " + img_path);
        }
        if (header->block_size != DEFAULT_BLOCK_SIZE) {
            throw UnsupportedBlockSizeError(
                "Unsupported block size, expected " +
                std::to_string(DEFAULT_BLOCK_SIZE) + ", got " +
                std::to_string(header->block_size));
        }
        header_ = *header;
    }

    ~LCBlockDevice() {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    LCSuperBlock get_super_block() const {
        return header_;
    }

    void read_block(uint32_t block_id, LCBlock &block) const {
        check_range(block_id, "read");
        off_t   off = static_cast<off_t>(block_id) * DEFAULT_BLOCK_SIZE;
        ssize_t n   = ::pread(fd_, block.data, DEFAULT_BLOCK_SIZE, off);
        LC_ASSERT(n == DEFAULT_BLOCK_SIZE, "pread failed");
    }

    void write_block(uint32_t block_id, const LCBlock &block) {
        check_range(block_id, "write");
        off_t   off = static_cast<off_t>(block_id) * DEFAULT_BLOCK_SIZE;
        ssize_t n   = ::pwrite(fd_, block.data, DEFAULT_BLOCK_SIZE, off);
        LC_ASSERT(n == DEFAULT_BLOCK_SIZE, "pwrite failed");
    }

private:
    void check_range(uint32_t id, const char *op) const {
        LC_ASSERT(
            id != 0,
            ("Cannot " + std::string(op) + " superblock (block 0)").c_str());
        LC_ASSERT(id < header_.total_blocks,
                  ("Block ID out of range: " + std::to_string(id)).c_str());
    }

    int          fd_ {-1};
    LCSuperBlock header_ {};
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_DEVICE_H
