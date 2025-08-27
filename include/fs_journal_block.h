#ifndef FS_JOURNAL_BLOCK_H
#define FS_JOURNAL_BLOCK_H

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

struct JournalSuper {
    be32 log_block_size;

    be64 len_blocks;
    be64 start_block;

    be32 sequence;  // First commit ID expected in log.

    be32 magic;

    u8 uuid[16];

    u8   csum_type;
    u8   csum_type_reserved[3];
    be32 csum;
};

FS_NAMESPACE_END

#endif  // FS_JOURNAL_BLOCK_H
