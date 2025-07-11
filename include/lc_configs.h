#ifndef LC_CONFIG_H
#define LC_CONFIG_H

// ==============================
//     LC Project Config Header
// ==============================
// This header centralizes feature macros for cross-platform,
// cross-compiler compatibility and consistent language-level behavior.
// It is safe to include globally in both library and application code.

// ==============================
//         Version Info
// ==============================

#define LC_VERSION_MAJOR 1
#define LC_VERSION_MINOR 0
#define LC_VERSION_PATCH 0
#define LC_VERSION_STR   "1.0.0"

// ==============================
//     C++ Standard Detection
// ==============================

#if defined(__cplusplus)

#  if !defined(__LC_CPP_STD_VER)
// Derive the standard version as an integer (e.g., 17 for C++17)
#    if __cplusplus <= 201103L
#      define __LC_CPP_STD_VER 11
#    elif __cplusplus <= 201402L
#      define __LC_CPP_STD_VER 14
#    elif __cplusplus <= 201703L
#      define __LC_CPP_STD_VER 17
#    elif __cplusplus <= 202002L
#      define __LC_CPP_STD_VER 20
#    elif __cplusplus <= 202302L
#      define __LC_CPP_STD_VER 23
#    else
#      define __LC_CPP_STD_VER 26  // future-proof
#    endif
#  endif

// ==============================
//     C++ Feature Macros
// ==============================

#  if __LC_CPP_STD_VER >= 11
#    define LC_ALIGNOF(_Tp)    alignof(_Tp)
#    define LC_ALIGNAS_TYPE(x) alignas(x)
#    define LC_ALIGNAS(x)      alignas(x)
#    define LC_NORETURN        [[noreturn]]
#    define LC_NOEXCEPT        noexcept
#    define LC_NOEXCEPT_(x)    noexcept(x)
#    define LC_CONSTEXPR       constexpr
#    if __LC_CPP_STD_VER >= 17
#      define LC_NODISCARD    [[nodiscard]]
#      define LC_FALLTHROUGH  [[fallthrough]]
#      define LC_MAYBE_UNUSED [[maybe_unused]]
#      define LC_EXPLICIT     explicit
#    else
#      define LC_NODISCARD
#      define LC_FALLTHROUGH
#      define LC_MAYBE_UNUSED
#      define LC_EXPLICIT explicit
#    endif
#  endif  // __LC_CPP_STD_VER >= 11

#endif    // defined(__cplusplus)

// ==============================
//     C Language Compatibility
// ==============================
#ifndef LC_EXTERN_C_BEGIN
#  ifdef __cplusplus
#    define LC_EXTERN_C_BEGIN \
        extern "C"            \
        {
#    define LC_EXTERN_C_END }
#  else
#    define LC_EXTERN_C_BEGIN
#    define LC_EXTERN_C_END
#  endif
#endif

// ==============================
//     Compiler Detection
// ==============================

#if defined(__clang__)
#  define LC_COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
#  define LC_COMPILER_GCC 1
#elif defined(_MSC_VER)
#  define LC_COMPILER_MSVC 1
#else
#  define LC_COMPILER_UNKNOWN 1
#endif

// ==============================
//     Platform Detection
// ==============================

#if defined(_WIN32) || defined(_WIN64)
#  define LC_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#  define LC_PLATFORM_MACOS 1
#elif defined(__linux__)
#  define LC_PLATFORM_LINUX 1
#else
#  define LC_PLATFORM_UNKNOWN 1
#endif

// ==============================
//     Function Attributes
// ==============================

// Force the compiler to always inline a function (if supported)
#if defined(__GNUC__) || defined(__clang__)
#  define LC_FORCE_INLINE inline __attribute__((always_inline))
#  define LC_LIKELY(x)    __builtin_expect(!!(x), 1)
#  define LC_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#  define LC_FORCE_INLINE __forceinline
#  define LC_LIKELY(x)    (x)
#  define LC_UNLIKELY(x)  (x)
#else
#  define LC_FORCE_INLINE inline
#  define LC_LIKELY(x)    (x)
#  define LC_UNLIKELY(x)  (x)
#endif

// Mark a function or symbol as deprecated with an optional message
#if __LC_CPP_STD_VER >= 14
#  define LC_DEPRECATED(msg) [[deprecated(msg)]]
#else
#  define LC_DEPRECATED(msg) __attribute__((deprecated(msg)))
#endif

// Export/Import macros for shared library symbols
#if defined(_WIN32)
#  define LC_EXPORT __declspec(dllexport)
#  define LC_IMPORT __declspec(dllimport)
#else
#  define LC_EXPORT __attribute__((visibility("default")))
#  define LC_IMPORT
#endif

// ==============================
//     Thread-Local Storage
// ==============================

#if __LC_CPP_STD_VER >= 11
#  define LC_THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
#  define LC_THREAD_LOCAL __declspec(thread)
#else
#  define LC_THREAD_LOCAL __thread
#endif

// ==============================
//     Namespace Helpers
// ==============================

#ifndef LC_NAMESPACE_BEGIN
#  define LC_NAMESPACE_BEGIN            namespace lc {
#  define LC_NAMESPACE_END              }
#  define LC_FILESYSTEM_NAMESPACE_BEGIN namespace fs {
#  define LC_FILESYSTEM_NAMESPACE_END   }
#endif

// ==============================
//     Debug Assertions
// ==============================

#if defined(DEBUG) || defined(_DEBUG)
#  ifdef __cplusplus
#    include <cstdio>
#    include <cstdlib>
#  else
#    include <stdio.h>
#    include <stdlib.h>
#  endif
#  define LC_ASSERT(condition, message)                           \
      do {                                                        \
          if (!(condition)) {                                     \
              std::fprintf(stderr,                                \
                           "Assertion failed: %s\nMessage: %s\n", \
                           #condition,                            \
                           message);                              \
              std::abort();                                       \
          }                                                       \
      } while (0)
#else
#  define LC_ASSERT(condition, message) ((void)0)
#endif

// ==============================
//     Internal Only Macros
// ==============================
// Symbols prefixed with __LC_ are for internal/private use only.

#define __LC_INTERNAL_VERSION (__LC_CPP_STD_VER * 100 + LC_VERSION_PATCH)

#endif  // LC_CONFIG_H
