#ifndef LC_MEMORY_H
#define LC_MEMORY_H

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

inline void *lc_malloc(size_t size) {
    return malloc(size);
}

inline void *lc_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

inline void *lc_realloc(void *ptr, size_t new_size) {
    return realloc(ptr, new_size);
}

inline void lc_free(void *ptr) {
    free(ptr);
}

template <typename T> T *lc_alloc_array(size_t count) {
    return new (std::nothrow) T[count];
}

template <typename T> void lc_free_array(T *ptr) {
    delete[] ptr;
}

inline std::atomic_flag *lc_alloc_atomic_flag_array(size_t count) {
    auto ptr = new (std::nothrow) std::atomic_flag[count];
    if (!ptr) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        ptr[i].clear();
    }
    return ptr;
}

inline void lc_free_atomic_flag_array(std::atomic_flag *ptr) {
    delete[] ptr;
}

inline void *lc_alloc_aligned(std::size_t size, std::size_t alignment) {
#if defined(_MSC_VER)
    return _aligned_malloc(size, alignment);
#elif defined(__APPLE__) || defined(__linux__)
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#else
    // fallback to regular malloc (not aligned)
    return std::malloc(size);
#endif
}

inline void lc_free_aligned(void *ptr) {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

inline void *lc_memset(void *ptr, int value, size_t num) {
    return memset(ptr, value, num);
}

inline void *lc_memcpy(void *dest, const void *src, size_t num) {
    return memcpy(dest, src, num);
}

inline void *lc_memmove(void *dest, const void *src, size_t num) {
    return memmove(dest, src, num);
}

inline int lc_memcmp(const void *ptr1, const void *ptr2, size_t num) {
    return memcmp(ptr1, ptr2, num);
}

inline const void *lc_memchr(const void *ptr, int value, size_t num) {
    return memchr(ptr, value, num);
}

inline size_t lc_strlen(const char *str) {
    return strlen(str);
}

inline int lc_strcmp(const char *str1, const char *str2) {
    return strcmp(str1, str2);
}

inline int lc_strncmp(const char *str1, const char *str2, size_t num) {
    return strncmp(str1, str2, num);
}

inline char *lc_strcpy(char *dest, const char *src) {
    return strcpy(dest, src);
}

inline char *lc_strncpy(char *dest, const char *src, size_t num) {
    return strncpy(dest, src, num);
}

inline char *lc_strcat(char *dest, const char *src) {
    return strcat(dest, src);
}

inline char *lc_strncat(char *dest, const char *src, size_t num) {
    return strncat(dest, src, num);
}

inline char *lc_strdup(const char *str) {
    return strdup(str);
}

inline void lc_strfree(char *str) {
    free(str);
}

#endif  // LC_MEMORY_H
