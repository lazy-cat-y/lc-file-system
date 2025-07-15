
#ifndef LC_UTILS_H
#define LC_UTILS_H

#include <cstdint>
#include "lc_configs.h"

inline uint32_t ceil_divide_int32_t(uint32_t a, uint32_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

inline uint64_t ceil_divide_int64_t(uint64_t a, uint64_t b) {
    LC_ASSERT(b != 0, "Division by zero");
    return a / b + (a % b != 0 ? 1 : 0);
}

#endif  // LC_UTILS_H