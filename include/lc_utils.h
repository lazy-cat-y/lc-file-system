
#ifndef LC_UTILS_H
#define LC_UTILS_H

#include <cstdint>

#include "lc_configs.h"

#define LC_BLOCK_ILLEGAL_ID 0xFFFFFFFF
#define LC_INODE_ILLEGAL_ID 0xFFFFFFFF

inline uint32_t lc_ceil_divide_int32_t(uint32_t a, uint32_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

inline uint64_t lc_ceil_divide_int64_t(uint64_t a, uint64_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

#endif  // LC_UTILS_H
