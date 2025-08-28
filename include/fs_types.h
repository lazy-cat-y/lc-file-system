#ifndef FS_TYPES_H
#define FS_TYPES_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fs_config.h"

FS_NAMESPACE_BEGIN

using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

using size_t = std::size_t;

using int8  = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;

using u8 = std::uint8_t;

// htole**()
// le**toh()
using le16 = std::uint16_t;
using le32 = std::uint32_t;
using le64 = std::uint64_t;

// htobe**()
// be**toh()
using be16 = std::uint16_t;
using be32 = std::uint32_t;
using be64 = std::uint64_t;

using string = std::string;

template <typename T>
using unique_ptr = std::unique_ptr<T>;
template <typename T>
using shared_ptr = std::shared_ptr<T>;
template <typename T>
using weak_ptr = std::weak_ptr<T>;

template <typename T>
using vector = std::vector<T>;

using thread = std::thread;

template <typename T>
using atomic = std::atomic<T>;

using condition_variable = std::condition_variable;
using mutex              = std::mutex;
template <typename T>
using lock_guard = std::lock_guard<T>;
template <typename T>
using unique_lock = std::unique_lock<T>;

FS_NAMESPACE_END

#endif  // FS_TYPES_H
