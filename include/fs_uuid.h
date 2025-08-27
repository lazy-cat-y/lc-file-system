#ifndef FS_UUID_H
#define FS_UUID_H

#include <random>

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

inline void generate_uuid_v4(uint8 *uuid) {
    static std::random_device                    rd;
    static std::mt19937                          gen(rd());
    static std::uniform_int_distribution<uint32> dis(0, 255);

    for (size_t i = 0; i < 16; ++i) {
        uuid[i] = static_cast<uint8>(dis(gen));
    }

    uuid[6] = (uuid[6] & 0x0F) | 0x40;  // Version 4

    uuid[8] = (uuid[8] & 0x3F) | 0x80;  // Variant 10
}

FS_NAMESPACE_END

#endif  // FS_UUID_H
