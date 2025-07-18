#include "lc_image_format.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "lc_block.h"
#include "lc_configs.h"
#include "lc_exception.h"
#include "lc_inode.h"
#include "lc_utils.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

void lc_format_image(const std::string &img_path,
                     const uint64_t     total_size_bytes) {
    LC_ASSERT(!(total_size_bytes & 0xFFF),
              "Total size must be a multiple of block size");

    // create file if not exists
    std::ofstream img_file = lc_create_empty_image(img_path, total_size_bytes);

    // initialize the block header(super block)
    LCSuperBlock header {};
    lc_initialize_super_block(total_size_bytes, header);

    // clear the image
    lc_clear_image(img_file, header.total_blocks);

    // write the super block to the first block
    lc_write_super_block(img_file, header);

    // initialize all inodes
    lc_initialize_inodes(img_file, header);
}

void lc_ensure_parent_directory_exists(const std::string &img_path) {
    std::filesystem::path path(img_path);
    std::filesystem::path parent_dir = path.parent_path();

    if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
        std::filesystem::create_directories(parent_dir);
    }
}

std::ofstream lc_create_empty_image(const std::string &img_path,
                                    const uint64_t     total_size_bytes) {
    namespace fs = std::filesystem;

    lc_ensure_parent_directory_exists(img_path);

    if (fs::exists(img_path)) {
        fs::remove(img_path);
    }

    std::ofstream img_file(img_path, std::ios::binary | std::ios::trunc);

    if (!img_file) {
        throw ImgCreationError("Failed to create image file: " + img_path);
    }
    return img_file;
}

void lc_initialize_super_block(const uint64_t total_size_bytes,
                               LCSuperBlock  &header) {
    LC_CONSTEXPR uint32_t bytes_per_inode = 16 * 1024;  // 16 KiB per inode
    uint32_t              total_blocks =
        static_cast<uint32_t>(total_size_bytes / DEFAULT_BLOCK_SIZE);

    uint32_t block_bitmap_size = lc_ceil_divide_int32_t(total_blocks, 8);  // 1
    uint32_t block_bitmap_block_count =
        lc_ceil_divide_int32_t(block_bitmap_size, DEFAULT_BLOCK_SIZE);

    uint32_t inode_count =
        static_cast<uint32_t>(total_size_bytes / bytes_per_inode);
    uint32_t inode_block_count =
        lc_ceil_divide_int32_t(inode_count, LC_INODES_PRE_BLOCK);
    uint32_t inode_bitmap_size = lc_ceil_divide_int32_t(inode_count, 8);
    uint32_t inode_bitmap_block_count =
        lc_ceil_divide_int32_t(inode_bitmap_size, DEFAULT_BLOCK_SIZE);

    header.total_blocks       = total_blocks;
    header.inode_count        = inode_count;
    header.block_bitmap_start = 1;  // Usually starts at block 1
    header.inode_bitmap_start =
        header.block_bitmap_start + block_bitmap_block_count;
    header.inode_block_start =
        header.inode_bitmap_start + inode_bitmap_block_count;
    header.inode_block_count = inode_block_count;
    header.data_start        = header.inode_block_start + inode_block_count;
}

void lc_write_super_block(std::ofstream &img_file, const LCSuperBlock &header) {
    LCBlock header_block {};
    block_clear(&header_block);
    block_write(&header_block, &header, sizeof(LCSuperBlock), 0);
    img_file.seekp(0);

    img_file.write(reinterpret_cast<char *>(block_as(&header_block)),
                   DEFAULT_BLOCK_SIZE);
}

void lc_clear_image(std::ofstream &img_file, const uint32_t total_blocks) {
    LCBlock zero_block {};
    block_clear(&zero_block);
    for (uint32_t i = 0; i < total_blocks; ++i) {
        img_file.write(reinterpret_cast<char *>(block_as(&zero_block)),
                       DEFAULT_BLOCK_SIZE);
    }
}

void lc_initialize_inodes(std::ofstream &img_file, const LCSuperBlock &header) {
    LCInode inode {};
    inode_clear(&inode);
    LCBlock inode_block {};
    block_clear(&inode_block);
    for (uint32_t i = 0; i < LC_INODES_PRE_BLOCK; ++i) {
        block_write(&inode_block, &inode, LC_INODE_SIZE, i * LC_INODE_SIZE);
    }
    for (uint32_t i = 0; i < header.inode_block_count; ++i) {
        img_file.seekp((header.inode_block_start + i) * DEFAULT_BLOCK_SIZE);
        img_file.write(reinterpret_cast<char *>(block_as(&inode_block)),
                       DEFAULT_BLOCK_SIZE);
    }
}

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END
