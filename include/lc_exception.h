

#ifndef LC_EXCEPTION_H
#define LC_EXCEPTION_H

#include <exception>
#include <iostream>
#include <new>
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

class UnsupportedImageFormatError : public FileSystemError {
public:
    LC_EXPLICIT UnsupportedImageFormatError(const std::string &format) :
        FileSystemError("Unsupported image format: " + format) {}
};

class UnsupportedBlockSizeError : public FileSystemError {
public:
    LC_EXPLICIT UnsupportedBlockSizeError(const std::string &message) :
        FileSystemError("Unsupported block size: " + message) {}
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

// ----------- Initialization System Errors -----------
class LCBadAllocError : public std::bad_alloc {
public:
    LC_EXPLICIT LCBadAllocError(const std::string &message) :
        std::bad_alloc(), message_(message) {}

    const char* what() const noexcept override {
        return message_.c_str();
    }

private:
    std::string message_;
};

/// ----------- Exception executor ------------
LC_NORETURN inline void lc_fatal_exception(const std::exception &e) {
    // TODO log to a file or console
    std::cerr << "Fatal error: " << e.what() << std::endl;
    std::terminate();
}


LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_EXCEPTION_H
