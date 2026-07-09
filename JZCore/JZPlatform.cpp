#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZStd.h"

#ifdef JZ_OS_NORMAL
#include <chrono>
#elif JZ_OS == JZ_OS_FREE_RTOS
#include "JZFreeRTOS.h"
#elif JZ_OS == JZ_OS_RT_THREAD
#include <rtthread.h>
#endif

static JZPlatformWrapper *g_inst = nullptr;
void JZPlatform::setInstance(JZPlatformWrapper *inst)
{
    g_inst = inst;
}

JZPlatformWrapper *JZPlatform::instance()
{
    return g_inst;
}

std::string JZPlatform::platform()
{
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

int64_t JZPlatform::getTickCountMs()
{
    if(g_inst)
        return (g_inst->getTickCountNs() / 1000000);

#ifdef JZ_OS_NORMAL
    // 最简写法：now() → time_since_epoch() → 毫秒数
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#elif JZ_OS == JZ_OS_FREE_RTOS
    constexpr double tick_ms = 1000.0 / configTICK_RATE_HZ;
    #if configSUPPORT_64_BIT_TICKS
        return (int64_t)(xTaskGetTickCount64() * tick_ms);
    #else
        return (int64_t)(xTaskGetTickCount() * tick_ms);
    #endif
#elif JZ_OS == JZ_OS_RT_THREAD
    constexpr double tick_ms = 1000.0 / RT_TICK_PER_SECOND;
    return (int64_t)(rt_tick_get() * tick_ms);
#else
    return 0;
#endif
}

int64_t JZPlatform::getTickCountNs()
{
    if(g_inst)
        return g_inst->getTickCountNs();

#ifdef JZ_OS_NORMAL
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#elif JZ_OS == JZ_OS_FREE_RTOS
    constexpr uint32_t tick_ns = 1000000000 / configTICK_RATE_HZ;
    #if configSUPPORT_64_BIT_TICKS
        return (int64_t)xTaskGetTickCount64() * tick_ns;
    #else
        return (int64_t)xTaskGetTickCount() * tick_ns;
    #endif
#elif JZ_OS == JZ_OS_RT_THREAD
    constexpr uint32_t tick_ns = 1000000000 / RT_TICK_PER_SECOND;
    return (int64_t)rt_tick_get() * tick_ns;
#else
    return 0;
#endif
}

#ifdef __ANDROID__

std::string JZPlatform::applicationDirPath()
{    
    return "/storage/emulated/0/Android/data/" + jzApp->packageName();
}

std::string JZPlatform::applicationDataPath()
{
    return "/storage/emulated/0/Android/data/" + jzApp->packageName() + "/files";
}

std::string JZPlatform::applicationTmpPath()
{
    return "/storage/emulated/0/Android/data/" + jzApp->packageName() + "/cache";
}

#elif defined(__linux__)

#include <unistd.h>  // 提供 readlink() 函数
#include <limits.h>  // 提供 PATH_MAX 常量（路径最大长度）

std::string JZPlatform::applicationFilePath()
{
    // 定义缓冲区存储程序完整路径，PATH_MAX 系统最大路径长度(4096)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

    // 处理所有读取失败/异常情况
    if (len == -1 || len == 0 || len >= sizeof(buffer) - 1)
    {
        return "";  // 读取失败/空路径/路径超长，返回空字符串
    }

    // 手动添加C字符串结束符，readlink不会自动添加，必须加
    buffer[len] = '\0';
    return buffer;
}

std::string JZPlatform::applicationDirPath() {
    // 定义缓冲区存储程序完整路径，PATH_MAX 系统最大路径长度(4096)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

    // 处理所有读取失败/异常情况
    if (len == -1 || len == 0 || len >= sizeof(buffer)-1) {
        return ""; // 读取失败/空路径/路径超长，返回空字符串
    }

    // 手动添加C字符串结束符，readlink不会自动添加，必须加
    buffer[len] = '\0';

    // ========== 核心修复逻辑：截取目录路径，剔除文件名 ==========
    // strrchr：从右向左查找第一个 '/' 分隔符的位置
    char* lastSlash = strrchr(buffer, '/');
    if (lastSlash != nullptr) {
        // 找到最后一个/，从该位置截断，只保留目录部分
        *lastSlash = '\0';
    }

    // 返回最终的【程序所在目录路径】
    return std::string(buffer);
}

std::string JZPlatform::applicationTmpPath()
{
    JZ_ASSERT(0);
    return std::string();
}

#elif defined(_WIN32)

std::string win32_applicationFilePath();
std::string win32_applicationDirPath();
std::string win32_applicationTmpPath();

std::string JZPlatform::applicationFilePath()
{
    return win32_applicationFilePath();
}

std::string JZPlatform::applicationDirPath()
{
    return win32_applicationDirPath();
}

std::string JZPlatform::applicationTmpPath()
{
    return win32_applicationTmpPath();
}

#else

std::string JZPlatform::applicationFilePath()
{
    return std::string();
}

std::string JZPlatform::applicationDirPath()
{
    return std::string();
}

std::string JZPlatform::applicationTmpPath()
{
    return std::string();
}

#endif
