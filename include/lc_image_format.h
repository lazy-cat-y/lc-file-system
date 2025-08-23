#ifndef LC_IMAGE_FORMAT_H
#define LC_IMAGE_FORMAT_H

#include <cstdint>
#include <fstream>
#include <string>

#include "lc_block.h"
#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

// write into the block bitmap.

void format_image(const std::string &img_path,
                     const uint64_t     total_size_bytes);

std::ofstream create_empty_image(const std::string &img_path,
                                    const uint64_t     total_size_bytes);

void ensure_parent_directory_exists(const std::string &img_path);

void initialize_super_block(const uint64_t total_size_bytes,
                               SuperBlock  &header);

void write_super_block(std::ofstream &img_file, const SuperBlock &header);

void clear_image(std::ofstream &img_file, const uint32_t total_blocks);

void initialize_inodes(std::ofstream &img_file, const SuperBlock &header);

void initialize_block_bitmap(std::ofstream      &img_file,
                                const SuperBlock &header);

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_IMAGE_FORMAT_H
