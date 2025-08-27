#ifndef FS_UTILS_H
#define FS_UTILS_H

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

static constexpr uint32 DEFAULT_BLOCK_SIZE = 4096;
static constexpr uint32 BLOCK_MAGIC_NUMBER = 0x4C435346u;

static constexpr uint8 SUPER_BLOCK_COUNT = 8;

static constexpr uint32 L_MAX_SIZE = 128 * 1024 * 1024;  // 128MiB max WAL size
static constexpr uint32 L_SEG_BLOCKS = 32;       // 128MiB per WAL segment

static constexpr uint32 DEFAULT_INODE_SIZE       = 256;  // bytes
static constexpr uint32 INODES_PER_BLOCK = DEFAULT_BLOCK_SIZE / DEFAULT_INODE_SIZE;

enum class CsumType : uint8 {
    None   = 0,
    CRC32C = 1,
};

FS_NAMESPACE_END

#endif  // FS_UTILS_H
