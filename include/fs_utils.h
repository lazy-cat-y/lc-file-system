#ifndef FS_UTILS_H
#define FS_UTILS_H

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

static constexpr uint32 DEFAULT_BLOCK_SIZE = 4096;
static constexpr uint32 BLOCK_MAGIC_NUMBER = 0x4C435346u;

static constexpr uint8 SUPER_BLOCK_COUNT = 8;

static constexpr uint32 L_MAX_SIZE = 128 * 1024 * 1024;  // 128MiB max WAL size
static constexpr uint32 L_SEG_BLOCKS = 32;         // 128MiB per WAL segment

static constexpr uint32 DEFAULT_INODE_SIZE = 256;  // bytes
static constexpr uint32 INODES_PER_BLOCK =
    DEFAULT_BLOCK_SIZE / DEFAULT_INODE_SIZE;

enum class CsumType : uint8 {
    None   = 0,
    CRC32C = 1,
};

#if defined(__linux__)
#  include <endian.h>
#elif defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#else
#  error "Unsupported platform"
#endif

static inline uint16_t to_be16(uint16_t x) {
#if defined(__linux__)
    return htobe16(x);
#elif defined(__APPLE__)
    return OSSwapHostToBigInt16(x);
#endif
}

static inline uint32_t to_be32(uint32_t x) {
#if defined(__linux__)
    return htobe32(x);
#elif defined(__APPLE__)
    return OSSwapHostToBigInt32(x);
#endif
}

static inline uint64_t to_be64(uint64_t x) {
#if defined(__linux__)
    return htobe64(x);
#elif defined(__APPLE__)
    return OSSwapHostToBigInt64(x);
#endif
}

static inline uint16_t to_le16(uint16_t x) {
#if defined(__linux__)
    return htole16(x);
#elif defined(__APPLE__)
    return OSSwapHostToLittleInt16(x);
#endif
}

static inline uint32_t to_le32(uint32_t x) {
#if defined(__linux__)
    return htole32(x);
#elif defined(__APPLE__)
    return OSSwapHostToLittleInt32(x);
#endif
}

static inline uint64_t to_le64(uint64_t x) {
#if defined(__linux__)
    return htole64(x);
#elif defined(__APPLE__)
    return OSSwapHostToLittleInt64(x);
#endif
}

static inline uint16_t from_be16(uint16_t x) {
#if defined(__linux__)
    return be16toh(x);
#elif defined(__APPLE__)
    return OSSwapBigToHostInt16(x);
#endif
}

static inline uint32_t from_be32(uint32_t x) {
#if defined(__linux__)
    return be32toh(x);
#elif defined(__APPLE__)
    return OSSwapBigToHostInt32(x);
#endif
}

static inline uint64_t from_be64(uint64_t x) {
#if defined(__linux__)
    return be64toh(x);
#elif defined(__APPLE__)
    return OSSwapBigToHostInt64(x);
#endif
}

static inline uint16_t from_le16(uint16_t x) {
#if defined(__linux__)
    return le16toh(x);
#elif defined(__APPLE__)
    return OSSwapLittleToHostInt16(x);
#endif
}

static inline uint32_t from_le32(uint32_t x) {
#if defined(__linux__)
    return le32toh(x);
#elif defined(__APPLE__)
    return OSSwapLittleToHostInt32(x);
#endif
}

static inline uint64_t from_le64(uint64_t x) {
#if defined(__linux__)
    return le64toh(x);
#elif defined(__APPLE__)
    return OSSwapLittleToHostInt64(x);
#endif
}

FS_NAMESPACE_END

#endif  // FS_UTILS_H
