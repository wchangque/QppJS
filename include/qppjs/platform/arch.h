#pragma once

#if defined(__x86_64__) || defined(_M_X64)
#define QPPJS_ARCH_X64 1
#define QPPJS_ARCH_NAME "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define QPPJS_ARCH_ARM64 1
#define QPPJS_ARCH_NAME "arm64"
#elif defined(__i386__) || defined(_M_IX86)
#define QPPJS_ARCH_X86 1
#define QPPJS_ARCH_NAME "x86"
#else
#define QPPJS_ARCH_UNKNOWN 1
#define QPPJS_ARCH_NAME "unknown"
#endif

static_assert(sizeof(void*) == 4 || sizeof(void*) == 8, "unsupported pointer width");
