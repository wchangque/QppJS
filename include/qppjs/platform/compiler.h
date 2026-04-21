#pragma once

#if defined(_MSC_VER)
#define QPPJS_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define QPPJS_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define QPPJS_ALWAYS_INLINE inline
#endif

#if defined(_MSC_VER)
#define QPPJS_UNREACHABLE() __assume(false)
#elif defined(__GNUC__) || defined(__clang__)
#define QPPJS_UNREACHABLE() __builtin_unreachable()
#else
#include <cstdlib>
#define QPPJS_UNREACHABLE() std::abort()
#endif

#if defined(__GNUC__) || defined(__clang__)
#define QPPJS_LIKELY(x) __builtin_expect(!!(x), 1)
#define QPPJS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define QPPJS_LIKELY(x) (x)
#define QPPJS_UNLIKELY(x) (x)
#endif

#if defined(_MSC_VER)
#define QPPJS_COMPILER "MSVC"
#elif defined(__clang__)
#define QPPJS_COMPILER "Clang"
#elif defined(__GNUC__)
#define QPPJS_COMPILER "GCC"
#else
#define QPPJS_COMPILER "Unknown"
#endif
