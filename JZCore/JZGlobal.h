#ifndef JZ_GLOBAL_H_
#define JZ_GLOBAL_H_

#include <assert.h>
#include <string>
#include <inttypes.h>

#define JZ_OS_WINDOWS   0
#define JZ_OS_LINUX     1
#define JZ_OS_FREE_RTOS 2
#define JZ_OS_RT_THREAD 3
#define JZ_OS_NONE      4

#ifndef JZ_OS
#if defined(_WIN32) || defined(_WIN64)
#define JZ_OS JZ_OS_WINDOWS
#elif defined(__linux__)
#define JZ_OS JZ_OS_LINUX
#else
#define JZ_OS JZ_OS_NONE
#endif
#endif


#define JZ_ASSERT(condition)          \
    do {                              \
        if (!(condition)) {           \
            jzassert_log(__FILE__, __LINE__, #condition);  \
        }                             \
    } while (0)

#define JZ_ASSERT_X(condition, desc)                      \
    do                                                    \
    {                                                     \
        if (!(condition))                                 \
        {                                                 \
            jzassert_log(__FILE__, __LINE__, desc);       \
        }                                                 \
    } while (0)

#ifndef NDEBUG
#define JZ_DEBUG_ASSERT(expr) JZ_ASSERT(expr)
#else
#define JZ_DEBUG_ASSERT(expr) ((void)0)
#endif

#define JZ_DISABLE_COPY(Class) \
    Class(const Class &) = delete; \
    Class &operator=(const Class &) = delete;

#if (JZ_OS == JZ_OS_WINDOWS || JZ_OS == JZ_OS_LINUX)
#define JZ_OS_NORMAL
#endif

#if defined(__GNUC__) || defined(__clang__)
#define JZ_UNREACHABLE() do { __builtin_trap(); __builtin_unreachable(); } while (0)
#elif defined(_MSC_VER)
#define JZ_UNREACHABLE() do { __debugbreak(); __assume(0); } while (0)
#else
#define JZ_UNREACHABLE() do { (void)0; abort(); } while (0)
#endif

#define JZ_FUNC_INFO __FUNCTION__

#define jz_signals public

namespace JZ
{
enum SplitBehavior
{
    KeepEmptyParts,  // 保留空字符串项
    SkipEmptyParts   // 跳过空字符串项
};
}

void jzassert_log(const char *file, int line, const char *message);

#endif