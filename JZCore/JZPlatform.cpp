#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZStd.h"
#include "JZDebug.h"
#include <cstdlib>
#include <stdlib.h>     // _dupenv_s / _putenv_s / getenv / setenv / unsetenv

#ifdef JZ_OS_NORMAL
#include <chrono>
#elif JZ_OS == JZ_OS_FREE_RTOS
#include "JZCore/private/JZFreeRTOS.h"
#elif JZ_OS == JZ_OS_RT_THREAD
#include <rtthread.h>
#endif

static JZPlatformWrapper *g_inst = nullptr;
JZPlatformWrapper::~JZPlatformWrapper()
{

}

void* JZPlatformWrapper::malloc(size_t size)
{
    return ::malloc(size);
}

void JZPlatformWrapper::free(void *ptr)
{
    ::free(ptr);
}

// === 环境变量的平台原始实现 ===
// 桌面/Android 走 OS; 裸机(FreeRTOS / RT-Thread)没有这个机制, 返回空/失败。
static std::string platformGetEnv(const std::string &name)
{
#ifdef JZ_OS_NORMAL
#  ifdef _MSC_VER
    // MSVC 下 getenv 属"不安全 CRT"(C4996), 用 _dupenv_s 更省事
    char *buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name.c_str()) != 0 || !buf)
        return std::string();

    std::string v(buf);
    ::free(buf);
    return v;
#  else
    const char *buf = ::getenv(name.c_str());
    return buf ? std::string(buf) : std::string();
#  endif
#else
    (void)name;
    return std::string();
#endif
}

static bool platformSetEnv(const std::string &name, const std::string &value)
{
    if (name.empty())
        return false;

#ifdef JZ_OS_NORMAL
#  ifdef _MSC_VER
    // Windows 下空 value 就是删除(_putenv_s 语义, 与 SetEnvironmentVariable 一致);
    // POSIX 的 setenv 把空串当有效值, 所以那边也显式 unsetenv 对齐。
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#  else
    if (value.empty())
        return ::unsetenv(name.c_str()) == 0;
    return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#  endif
#else
    (void)value;
    return false;
#endif
}

std::string JZPlatformWrapper::getEnv(const std::string &name)
{
    return platformGetEnv(name);
}

bool JZPlatformWrapper::setEnv(const std::string &name, const std::string &value)
{
    return platformSetEnv(name, value);
}

//JZPlatform
void JZPlatform::setInstance(JZPlatformWrapper *inst)
{
    if (g_inst && g_inst != inst)
    {
        if (inst)
        {
            jzWarning("JZPlatformWrapper instance already set, replacing old driver");
        }
        delete g_inst;
    }
    g_inst = inst;
}

JZPlatformWrapper *JZPlatform::instance()
{
    return g_inst;
}

void* JZPlatform::malloc(size_t size)
{
    if (g_inst)
        return g_inst->malloc(size);
        
    return ::malloc(size);
}

void JZPlatform::free(void *ptr)
{
    if (g_inst)
        g_inst->free(ptr);
    else
        ::free(ptr);
}

std::string JZPlatform::getEnv(const std::string &name)
{
    if (g_inst)
        return g_inst->getEnv(name);
    return platformGetEnv(name);
}

bool JZPlatform::setEnv(const std::string &name, const std::string &value)
{
    if (g_inst)
        return g_inst->setEnv(name, value);
    return platformSetEnv(name, value);
}

// ==================== 平台信息 ====================

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

#include <unistd.h>
#include <limits.h>

std::string JZPlatform::applicationFilePath()
{
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len == -1 || len == 0 || len >= sizeof(buffer) - 1)
        return "";
    buffer[len] = '\0';
    return buffer;
}

std::string JZPlatform::applicationDirPath() {
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len == -1 || len == 0 || len >= sizeof(buffer)-1)
        return "";
    buffer[len] = '\0';
    char* lastSlash = strrchr(buffer, '/');
    if (lastSlash != nullptr)
        *lastSlash = '\0';
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

std::string JZPlatform::applicationFilePath()  { return std::string(); }
std::string JZPlatform::applicationDirPath()   { return std::string(); }
std::string JZPlatform::applicationTmpPath()   { return std::string(); }

#endif
