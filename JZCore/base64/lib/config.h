#ifndef BASE64_CONFIG_H
#define BASE64_CONFIG_H

/* Static config.h: replaces the CMake-generated version of the original library.
 * HAVE_* only means "this codec is compiled in"; whether it is actually used is
 * decided by runtime CPU feature detection. We enable the codecs for the target
 * architecture directly, so no CMake generation step is needed. */

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  #define HAVE_SSSE3  1
  #define HAVE_SSE41  1
  #define HAVE_SSE42  1
  #define HAVE_AVX    1
  #define HAVE_AVX2   1
  #define HAVE_AVX512 0   /* 闭关: 避免对老版本 MSVC 要求 /arch:AVX512 */
  #define HAVE_NEON32 0
  #define HAVE_NEON64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
  #define HAVE_SSSE3  0
  #define HAVE_SSE41  0
  #define HAVE_SSE42  0
  #define HAVE_AVX    0
  #define HAVE_AVX2   0
  #define HAVE_AVX512 0
  #define HAVE_NEON32 0
  #define HAVE_NEON64 1
#elif defined(_M_ARM) || defined(__arm__)
  #define HAVE_SSSE3  0
  #define HAVE_SSE41  0
  #define HAVE_SSE42  0
  #define HAVE_AVX    0
  #define HAVE_AVX2   0
  #define HAVE_AVX512 0
  #define HAVE_NEON32 1
  #define HAVE_NEON64 0
#else
  #define HAVE_SSSE3  0
  #define HAVE_SSE41  0
  #define HAVE_SSE42  0
  #define HAVE_AVX    0
  #define HAVE_AVX2   0
  #define HAVE_AVX512 0
  #define HAVE_NEON32 0
  #define HAVE_NEON64 0
#endif

#endif /* BASE64_CONFIG_H */
