#ifndef FS_IMAGE_FORMAT_H
#define FS_IMAGE_FORMAT_H

#include "fs_config.h"
#include "fs_error.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

ImgStatue create_fs_image(const string& path, const uint32 total_size);

FS_NAMESPACE_END

#endif  // FS_IMAGE_FORMAT_H
