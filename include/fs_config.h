#ifndef FS_CONFIG_H
#define FS_CONFIG_H

#define FS_VERSION_MAJOR 1
#define FS_VERSION_MINOR 1
#define FS_VERSION_PATCH 0
#define FS_VERSION_STR   "1.1.0"

#if defined(__GNUC__) || defined(__clang__)
#  define FORCE_INLINE inline __attribute__((always_inline))
#  define LIKELY(x)    __builtin_expect(!!(x), 1)
#  define UNLIKELY(x)  __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#  define FORCE_INLINE __forceinline
#  define LIKELY(x)    (x)
#  define UNLIKELY(x)  (x)
#else
#  define FORCE_INLINE inline
#  define LIKELY(x)    (x)
#  define UNLIKELY(x)  (x)
#endif

#if defined(DEBUG) || defined(_DEBUG)
#  ifdef __cplusplus
#    include <cstdio>
#    include <cstdlib>
#  else
#    include <stdio.h>
#    include <stdlib.h>
#  endif
#  define ASSERT(condition, message)                                           \
      do {                                                                     \
          if (!(condition)) {                                                  \
              std::fprintf(stderr,                                             \
                           "Assertion failed: %s\nMessage: %s\n",              \
                           #condition,                                         \
                           message);                                           \
              std::abort();                                                    \
          }                                                                    \
      } while (0)
#else
#  define ASSERT(condition, message) ((void)0)
#endif

#ifndef FS_NAMESPACE_BEGIN
#  define FS_NAMESPACE_BEGIN                                                   \
      namespace lc {                                                           \
      namespace fs {
#  define FS_NAMESPACE_END                                                     \
      }                                                                        \
      }
#endif

#endif  // FS_CONFIG_H
