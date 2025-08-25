#ifndef FS_ERROR_H
#define FS_ERROR_H

#include "fs_config.h"

FS_NAMESPACE_BEGIN

enum class ImgStatue {
    Success,
    CreationFailed,
    AlreadyExists,
    NotExist,
};

FS_NAMESPACE_END

#endif  // FS_ERROR_H
