#ifndef FS_TYPES_H
#define FS_TYPES_H

#include <cstdint>
#include <memory>

#include "fs_config.h"

FS_NAMESPACE_BEGIN

using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using size_t = std::size_t;

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

using string = std::string;

template <typename T>
using unique_ptr = std::unique_ptr<T>;
template <typename T>
using shared_ptr = std::shared_ptr<T>;
template <typename T>
using weak_ptr = std::weak_ptr<T>;

FS_NAMESPACE_END

#endif  // FS_TYPES_H
