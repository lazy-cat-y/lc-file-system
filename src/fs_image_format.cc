
#include <endian.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

#include "fs_block.h"
#include "fs_config.h"
#include "fs_crc32c.h"
#include "fs_error.h"
#include "fs_image_creation.h"
#include "fs_journal_block.h"
#include "fs_memory.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

ImgCreateStatue create_volume(VolumeParams       &params,
                              CreateVolumeResult *result) {
    if (params.img_path.empty() ||
        params.img_size_bytes < params.block_size_bytes) {
        return ImgCreateStatue::InvalidParams;
    }
    if ((params.block_size_bytes & (params.block_size_bytes - 1)) != 0) {
        return ImgCreateStatue::InvalidParams;
    }
    if (params.group_size_bytes < params.block_size_bytes) {
        return ImgCreateStatue::InvalidParams;
    }

    int img_fd =
        ::open(params.img_path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    if (img_fd < 0) {
        close(img_fd);
        return ImgCreateStatue::CreationFailed;
    }
    if (ftruncate(img_fd, params.img_size_bytes) != 0) {
        close(img_fd);
        return ImgCreateStatue::CreationFailed;
    }

    const uint64 blocks_total = params.img_size_bytes / params.block_size_bytes;
    const uint32 blocks_per_group =
        params.group_size_bytes / params.block_size_bytes;
    const uint64 first_data_block = params.first_data_block;
    const uint64 data_blocks      = (blocks_total > first_data_block)
                                        ? (blocks_total - first_data_block)
                                        : 0;
    const uint64 groups =
        (data_blocks + blocks_per_group - 1) / blocks_per_group;
    const uint64 inodes_total =
        (params.inode_count
             ? params.inode_count
             : std::max<uint64_t>(
                   groups * (params.group_size_bytes / params.inode_ratio),
                   params.reserved_inodes));
    const uint64 inodes_per_group = (inodes_total + groups - 1) / groups;
    const uint32 log_block_size   = __builtin_ctz(params.block_size_bytes);
    const uint16 log_inode_size   = __builtin_ctz(params.inode_size_bytes);

    uint32 incompat_flags = 0;
    if (params.journal_params.type != JournalType::None) {
        incompat_flags |=
            static_cast<uint32>(IncompatType::INCOMPAT_HAS_JOURNAL);
    }
    if (params.journal_params.type == JournalType::External) {
        incompat_flags |=
            static_cast<uint32>(IncompatType::INCOMPAT_EXTERNAL_JOURNAL);
    } else {
        incompat_flags |= static_cast<uint32>(IncompatType::INCOMPAT_64BIT);
        ::close(img_fd);
        return ImgCreateStatue::Unsupported;
    }

    SuperBlock sb {};

    sb.inode_count      = htole32(inodes_total);
    sb.block_count      = htole64(blocks_total);
    sb.free_inode_count = htole32(inodes_total);
    sb.free_block_count = htole64(data_blocks);

    sb.first_data_block = htole64(first_data_block);

    sb.log_block_size = htole32(log_block_size);

    sb.block_pre_group = htole32(blocks_per_group);
    sb.inode_pre_group = htole32(inodes_per_group);

    sb.log_inode_size      = htole32(log_inode_size);
    sb.inode_size_reserved = 0;

    sb.magic = htole32(params.magic);

    fs_memcpy(sb.uuid, params.uuid, 16);

    sb.mtime = htole64(0);
    sb.wtime = htole64(0);

    sb.incompat_flags = htole32(incompat_flags);
    sb.jnl_type       = static_cast<u8>(params.journal_params.type);
    sb.jnl_csum_type  = static_cast<u8>(params.csum_type);
    sb.jnl_reserved   = 0;

    if (params.journal_params.type == JournalType::External) {
        fs_memcpy(sb.jnl.external.jnl_uuid, params.journal_params.jnl_uuid, 16);
    } else {
        ::close(img_fd);
        return ImgCreateStatue::Unsupported;
    }

    sb.csum_type = static_cast<u8>(params.csum_type);
    fs_memset(sb.csum_reserved, 0, sizeof(sb.csum_reserved));
    sb.csum = 0;

    uint32 crc = crc32c(&sb, sizeof(sb));
    sb.csum    = htole32(crc);

    if (pwrite(img_fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb)) {
        close(img_fd);
        return ImgCreateStatue::CreationFailed;
    }

    // initialize the journal
    uint64 jnl_blocks = 0;

    fsync(img_fd);
    close(img_fd);
    if (params.journal_params.type == JournalType::External) {
        int jnl_fd = ::open(params.journal_params.jnl_path.c_str(),
                            O_CREAT | O_EXCL | O_RDWR,
                            0644);
        if (jnl_fd < 0) {
            close(img_fd);
            return ImgCreateStatue::CreationFailed;
        }
        if (ftruncate(jnl_fd, params.external_journal_size_bytes) != 0) {
            close(jnl_fd);
            return ImgCreateStatue::CreationFailed;
        }

        jnl_blocks = params.external_journal_size_bytes /
                     params.journal_params.jnl_block_size_bytes;
        be32 log_block_size =
            htobe32(__builtin_ctz(params.journal_params.jnl_block_size_bytes));
        be64 len_blocks  = htobe64(jnl_blocks);
        be64 start_block = htobe64(params.journal_params.start_block);
        be32 magic       = htobe32(params.journal_params.jnl_magic);

        JournalSuper js {};
        js.log_block_size = log_block_size;

        js.len_blocks  = len_blocks;
        js.start_block = start_block;

        js.sequence = 0;

        js.magic = magic;

        fs_memcpy(js.uuid, params.journal_params.jnl_uuid, 16);

        js.csum_type = static_cast<u8>(params.journal_params.jnl_csum);
        fs_memset(js.csum_type_reserved, 0, sizeof(js.csum_type_reserved));

        be32 csum = htobe32(crc32c(&js, sizeof(js)));

        if (pwrite(jnl_fd, &js, sizeof(js), 0) != (ssize_t)sizeof(js)) {
            close(jnl_fd);
            return ImgCreateStatue::CreationFailed;
        }

        fsync(jnl_fd);
        close(jnl_fd);
    } else {
        close(img_fd);
        return ImgCreateStatue::Unsupported;
    }

    if (result) {
        result->blocks_total       = blocks_total;
        result->blocks_per_group   = blocks_per_group;
        result->groups             = groups;
        result->inodes_total       = inodes_total;
        result->inodes_per_group   = inodes_per_group;
        result->super_offset_block = 0;
        result->jnl_blocks         = jnl_blocks;
        result->jnl_type           = params.journal_params.type;
    }

    return ImgCreateStatue::Success;
}

FS_NAMESPACE_END
