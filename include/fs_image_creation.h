#ifndef FS_IMAGE_FORMAT_H
#define FS_IMAGE_FORMAT_H

#include <string>

#include "fs_block.h"
#include "fs_config.h"
#include "fs_error.h"
#include "fs_types.h"
#include "fs_utils.h"

FS_NAMESPACE_BEGIN

struct JournalParams {
    JournalType type = JournalType::None;

    uint32 jnl_block_size_bytes;

    uint32 jnl_magic;
    uint64 start_block;

    // type == Inode
    uint64_t jnl_inode = 0;

    // type == FixedArea
    uint64_t jnl_start_block = 0;
    uint64_t jnl_len_blocks  = 0;

    // type == External
    uint8       jnl_uuid[16];
    std::string jnl_path;

    CsumType jnl_csum = CsumType::CRC32C;
};

struct VolumeParams {
    std::string img_path;
    uint64      img_size_bytes;
    uint32      block_size_bytes = DEFAULT_BLOCK_SIZE;

    uint32 inode_size_bytes = DEFAULT_INODE_SIZE;
    uint32 inode_count;
    uint64 inode_ratio;

    uint32 group_size_bytes;

    JournalParams journal_params;
    uint64        external_journal_size_bytes;

    uint8    uuid[16];
    uint32   magic;
    CsumType csum_type;

    uint64 first_data_block;
    uint32 reserved_inodes;
};

struct CreateVolumeResult {
    uint64_t    blocks_total;
    uint64_t    blocks_per_group;
    uint64_t    groups;
    uint64_t    inodes_total;
    uint64_t    inodes_per_group;
    uint64_t    super_offset_block;
    uint64_t    jnl_blocks;
    JournalType jnl_type;
};

ImgCreateStatue create_volume(VolumeParams &params, CreateVolumeResult *result);

FS_NAMESPACE_END

#endif  // FS_IMAGE_FORMAT_H
