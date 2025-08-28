#ifndef FS_MEMORY_H
#define FS_MEMORY_H

#include <atomic>
#include <cstring>

#include "fs_config.h"
#include "fs_types.h"

FS_NAMESPACE_BEGIN

using MemOrder = std::memory_order;

inline void *fs_memset(void *ptr, int value, size_t num) {
    return memset(ptr, value, num);
}

inline void *fs_memcpy(void *dest, const void *src, size_t num) {
    return memcpy(dest, src, num);
}

inline void *fs_memmove(void *dest, const void *src, size_t num) {
    return memmove(dest, src, num);
}

inline int fs_memcmp(const void *ptr1, const void *ptr2, size_t num) {
    return memcmp(ptr1, ptr2, num);
}

inline const void *fs_memchr(const void *ptr, int value, size_t num) {
    return memchr(ptr, value, num);
}

inline size_t fs_strlen(const char *str) {
    return strlen(str);
}

inline int fs_strcmp(const char *str1, const char *str2) {
    return strcmp(str1, str2);
}

inline int fs_strncmp(const char *str1, const char *str2, size_t num) {
    return strncmp(str1, str2, num);
}

inline char *fs_strcpy(char *dest, const char *src) {
    return strcpy(dest, src);
}

inline char *fs_strncpy(char *dest, const char *src, size_t num) {
    return strncpy(dest, src, num);
}

inline char *fs_strcat(char *dest, const char *src) {
    return strcat(dest, src);
}

inline char *fs_strncat(char *dest, const char *src, size_t num) {
    return strncat(dest, src, num);
}

inline char *fs_strdup(const char *str) {
    return strdup(str);
}

inline void fs_strfree(char *str) {
    free(str);
}

FS_NAMESPACE_END

#endif  // FS_MEMORY_H
