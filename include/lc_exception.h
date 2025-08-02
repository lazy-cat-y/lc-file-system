

#ifndef LC_EXCEPTION_H
#define LC_EXCEPTION_H

#ifndef __cplusplus
#  error "lc_exception.h is a C++ header and must be compiled as C++"
#endif

#include <cstdint>
#include <stdexcept>
#include <string>

#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

class FileSystemError : public std::runtime_error {
public:
    LC_EXPLICIT FileSystemError(const std::string &message) :
        std::runtime_error(message) {}
};

// ----------- File Operation Errors -----------
class LCInvalidFileLenError : public FileSystemError {
public:
    LC_EXPLICIT LCInvalidFileLenError(uint64_t len) :
        FileSystemError("Invalid file length: " + std::to_string(len)) {}
};

class LCFileExistsError : public FileSystemError {
public:
    LC_EXPLICIT LCFileExistsError(const std::string &filename) :
        FileSystemError("File already exists: " + filename) {}
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_EXCEPTION_H
