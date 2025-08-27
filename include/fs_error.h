#ifndef FS_ERROR_H
#define FS_ERROR_H

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

enum class ImgCreateStatue : uint32 {
    Success,
    InvalidParams,
    Unsupported,
    CreationFailed,
    AlreadyExists,
    NotExist,
};

FS_NAMESPACE_END

#endif  // FS_ERROR_H
