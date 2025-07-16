

#ifndef LC_BLOCK_BUFFER_H
#define LC_BLOCK_BUFFER_H

#include <algorithm>
#include <cstdint>

#include "lc_configs.h"
#include "lc_inode.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

typedef struct LCInodeBufferPoolFrame {
    bool     valid;
    bool     dirty;
    uint8_t  ref_count;
    uint8_t  usage_count;
    uint32_t inode_id;
    uint32_t block_id;  // Block ID where the inode is stored
    LCInode  inode;     // The inode data
} LCInodeBufferPoolFrame;

inline uint8_t add_lc_inode_usage_count(uint8_t usage_count) {
    return std::min<uint8_t>(usage_count + 1, static_cast<uint8_t>(5));
}

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_BLOCK_BUFFER_H
