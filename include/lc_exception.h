

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

class FileNotFoundError : public FileSystemError {
public:
    LC_EXPLICIT FileNotFoundError(const std::string &filename) :
        FileSystemError("File not found: " + filename) {}
};

class FileAlreadyExistsError : public FileSystemError {
public:
    LC_EXPLICIT FileAlreadyExistsError(const std::string &filename) :
        FileSystemError("File already exists: " + filename) {}
};

class NotADirectoryError : public FileSystemError {
public:
    LC_EXPLICIT NotADirectoryError(const std::string &path) :
        FileSystemError("Not a directory: " + path) {}
};

class NotAFileError : public FileSystemError {
public:
    LC_EXPLICIT NotAFileError(const std::string &path) :
        FileSystemError("Not a file: " + path) {}
};

class PermissionDeniedError : public FileSystemError {
public:
    LC_EXPLICIT PermissionDeniedError(const std::string &path) :
        FileSystemError("Permission denied: " + path) {}
};

class PathError : public FileSystemError {
public:
    LC_EXPLICIT PathError(const std::string &path) :
        FileSystemError("Invalid path: " + path) {}
};

class ImgCreationError : public FileSystemError {
public:
    LC_EXPLICIT ImgCreationError(const std::string &message) :
        FileSystemError("Image creation error: " + message) {}
};

class ImgOpenError : public FileSystemError {
public:
    LC_EXPLICIT ImgOpenError(const std::string &message) :
        FileSystemError("Image open error: " + message) {}
};

class ImgMagicError : public FileSystemError {
public:
    LC_EXPLICIT ImgMagicError(const std::string &message) :
        FileSystemError("Image magic error: " + message) {}
};

class ImgSizeMismatchError : public FileSystemError {
public:
    LC_EXPLICIT ImgSizeMismatchError(const std::string &message) :
        FileSystemError("Image size mismatch: " + message) {}
};

class UnsupportedBlockSizeError : public FileSystemError {
public:
    LC_EXPLICIT UnsupportedBlockSizeError(const std::string &message) :
        FileSystemError("Unsupported block size: " + message) {}
};

class FileSystemInitializationError : public FileSystemError {
public:
    LC_EXPLICIT FileSystemInitializationError(const std::string &message) :
        FileSystemError("File system initialization error: " + message) {}
};

class FileSystemOperationError : public FileSystemError {
public:
    LC_EXPLICIT FileSystemOperationError(const std::string &operation,
                                         const std::string &message) :
        FileSystemError("File system operation error: " + operation + " - " +
                        message) {}
};

class BlockIdOutOfRangeError : public FileSystemError {
public:
    LC_EXPLICIT BlockIdOutOfRangeError(uint32_t block_id,
                                       uint32_t max_block_id) :
        FileSystemError("Block ID out of range: " + std::to_string(block_id) +
                        ", max: " + std::to_string(max_block_id)) {}
};

class ImgReadError : public FileSystemError {
public:
    LC_EXPLICIT ImgReadError(const std::string &message) :
        FileSystemError("Image read error: " + message) {}
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_EXCEPTION_H
