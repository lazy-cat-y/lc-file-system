#ifndef LC_IMAGE_FORMAT_H
#define LC_IMAGE_FORMAT_H

#include <cstdint>
#include <fstream>
#include <string>

#include "lc_block.h"
#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

// TODO: sign the bitmap blocks, inode blocks and super blocks as allocated and
// write into the block bitmap.

void lc_format_image(const std::string &img_path,
                     const uint64_t     total_size_bytes);

std::ofstream lc_create_empty_image(const std::string &img_path,
                                    const uint64_t     total_size_bytes);

void lc_ensure_parent_directory_exists(const std::string &img_path);

void lc_initialize_super_block(const uint64_t total_size_bytes,
                               LCSuperBlock  &header);

void lc_write_super_block(std::ofstream &img_file, const LCSuperBlock &header);

void lc_clear_image(std::ofstream &img_file, const uint32_t total_blocks);

void lc_initialize_inodes(std::ofstream &img_file, const LCSuperBlock &header);

void lc_initialize_block_bitmap(std::ofstream      &img_file,
                                const LCSuperBlock &header);

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_IMAGE_FORMAT_H
