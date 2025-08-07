
#ifndef LC_UTILS_H
#define LC_UTILS_H

#include <algorithm>
#include <cstdint>

#include "lc_configs.h"

#define DEFAULT_BLOCK_SIZE 4096
#define LC_BITS_PER_BLOCK  32768  // 4096 * 8 bits

#define LC_BLOCK_ILLEGAL_ID  0xFFFFFFFF
#define LC_INODE_ILLEGAL_ID  0xFFFFFFFF
#define LC_CURRUNT_DIRECTORY "."
#define LC_PARENT_DIRECTORY  ".."

inline uint32_t lc_ceil_divide_int32_t(uint32_t a, uint32_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

inline uint64_t lc_ceil_divide_int64_t(uint64_t a, uint64_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

inline uint32_t *lc_uint8_array_to_uint32_array(uint8_t *array) {
    return reinterpret_cast<uint32_t *>(array);
}

inline uint8_t lc_add_usage_count(uint8_t usage_count,
                                  uint8_t max_usage_count) {
    return std::min<uint8_t>(usage_count + 1, max_usage_count);
}

#endif  // LC_UTILS_H
