
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "fs_block.h"
#include "fs_config.h"
#include "fs_crc32c.h"
#include "fs_image_format.h"
#include "fs_types.h"
#include "fs_utils.h"

FS_NAMESPACE_BEGIN

ImgStatue create_fs_image(const string &path, const uint32 total_size_bytes) {
    ASSERT(!(total_size_bytes & 0xFFF),
           "Total size must be a multiple of block size");

    namespace stdfs = std::filesystem;

    stdfs::path image_path(path);
    stdfs::path parent_path = image_path.parent_path();

    if (!parent_path.empty() && !stdfs::exists(parent_path)) {
        if (!stdfs::create_directories(parent_path)) {
            return ImgStatue::CreationFailed;
        }
    }

    if (stdfs::exists(image_path)) {
        return ImgStatue::AlreadyExists;
    }

    using stdof = std::ofstream;

    stdof image_file(path, stdof::binary | stdof::trunc);

    if (!image_file) {
        return ImgStatue::CreationFailed;
    }

    uint32 total_blocks = total_size_bytes / DEFAULT_BLOCK_SIZE;

    Block zero_block {};
    block_clear(&zero_block);
    for (uint32 i = 0; i < total_blocks; i++) {
        image_file.seekp(i * DEFAULT_BLOCK_SIZE, std::ios::beg);
        image_file.write(reinterpret_cast<const char *>(&zero_block), DEFAULT_BLOCK_SIZE);
    }

    // Initialize super block
    SuperBlock       super_block {};
    constexpr uint32 bytes_per_inode = 16 * 1024;

    uint32 l_start = SUPER_BLOCK_COUNT;
    uint32 l_total_size =
        std::max<uint32>((total_size_bytes / 32 + DEFAULT_BLOCK_SIZE - 1) &
                             ~(DEFAULT_BLOCK_SIZE - 1),
                         L_MAX_SIZE);
    uint32 l_total_blocks = l_total_size / DEFAULT_BLOCK_SIZE;

    uint32 block_bitmap_start = l_start + l_total_blocks;
    uint32 block_bitmap_size =
        (total_blocks / 8) + ((total_blocks % 8) ? 1 : 0);
    uint32 block_bitmap_blocks =
        (block_bitmap_size / DEFAULT_BLOCK_SIZE) +
        ((block_bitmap_size % DEFAULT_BLOCK_SIZE) ? 1 : 0);

    uint32 inode_count = total_size_bytes / bytes_per_inode;

    uint32 inode_bitmap_start = block_bitmap_start + block_bitmap_blocks;
    uint32 inode_bitmap_size  = (inode_count / 8) + ((inode_count % 8) ? 1 : 0);
    uint32 inode_bitmap_blocks =
        (inode_bitmap_size / DEFAULT_BLOCK_SIZE) +
        ((inode_bitmap_size % DEFAULT_BLOCK_SIZE) ? 1 : 0);

    uint32 inode_block_count = (inode_count / INODES_PER_BLOCK) +
                               ((inode_count % INODES_PER_BLOCK) ? 1 : 0);
    uint32 inode_block_start = inode_bitmap_start + inode_bitmap_blocks;

    uint32 data_start = inode_block_start + inode_block_count;

    super_block.img_total_blocks   = total_blocks;
    super_block.l_start            = l_start;
    super_block.l_total_blocks     = l_total_blocks;
    super_block.block_bitmap_start = block_bitmap_start;
    super_block.inode_count        = inode_count;
    super_block.inode_bitmap_start = inode_bitmap_start;
    super_block.inode_block_count  = inode_block_count;
    super_block.inode_block_start  = inode_block_start;
    super_block.data_start         = data_start;
    super_block.crc                = 0;

    Block block {};
    block_clear(&block);
    block_write(&block,
                reinterpret_cast<const char *>(&super_block),
                sizeof(super_block),
                0);

    uint32 crc = crc32c(block_as(&block), DEFAULT_BLOCK_SIZE);

    super_block.crc = crc;
    block_write(&block,
                reinterpret_cast<const char *>(&super_block),
                sizeof(super_block),
                0);

    return ImgStatue::Success;
}

FS_NAMESPACE_END
