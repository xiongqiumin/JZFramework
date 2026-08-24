#include "JZPlatform.h"
#include "JZApplication.h"
#include "JZStd.h"
#include "JZDebug.h"
#include <cstdlib>

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
