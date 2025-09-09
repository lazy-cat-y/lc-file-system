#ifndef FS_JOURNAL_BLOCK_H
#define FS_JOURNAL_BLOCK_H

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

static constexpr uint32 JOURNAL_SUPERBLOCK_MAGIC   = 0x4C434A53u;
static constexpr uint32 JOURNAL_DEFAULT_BLOCK_SIZE = 4096;

enum class JournalBlockType : uint32 {
    DesriptorBlock       = 1,
    BlockCommitRecord    = 2,
    JounralSuperBlock    = 3,
    BlockRevocationBlock = 4
};

struct JournalBlockHeader {
    be32 magic;
    be32 block_type;
    be32 sequence;
};

struct JournalSuper {
    JournalBlockHeader header;

    be32 log_block_size;

    be64 s_first;
    be64 len_blocks;

    be64 start_block;

    u8 uuid[16];

    u8   csum_type;
    u8   csum_type_reserved[3];
    be32 csum;
};

struct JournalBlockTag {
    be64 block_number;
    be32 csum;  // jounral uuid + block number + block data
};

// struct JournalBlockDescriptor {
//     JournalBlockHeader header;

//     unique_ptr<JournalBlockTag[]> tags;
// };

FS_NAMESPACE_END

#endif  // FS_JOURNAL_BLOCK_H
