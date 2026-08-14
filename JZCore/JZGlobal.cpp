#include <stdlib.h>
#include <string>
#include <vector>
#include "JZGlobal.h"
#include "jz_version.h"
#include "JZStd.h"
#include "JZLog.h"

// MG_TLS 定义于 mongoose.h, 仅启用 JZNet 时包含并打印 TLS 信息
#ifdef HAS_JZNet
#include "mongoose.h"
#endif

void jzassert_log(const char *file, int line, const char *message)
{
    LOG_FMT_F("%s(%d): %s",file,line,message);
}

// 返回 mongoose 的 TLS 后端 + 平台后端信息, 形如 "MG_TLS=3(builtin), MG_ARCH=1(UNIX)"
// 参考 mongoose.h 中的 MG_TLS / MG_ARCH 宏映射。仅在启用 JZNet (已包含 mongoose.h) 时有效。
#ifdef HAS_JZNet
static const char *jzMgTlsName()
{
    switch (MG_TLS)
    {
    case 0: return "none";
    case 1: return "MbedTLS";
    case 2: return "OpenSSL";
    case 3: return "builtin";
    case 4: return "custom";
    case 5: return "WolfSSL";
    default: return "unknown";
    }
}

static const char *jzMgArchName()
{
    switch (MG_ARCH)
    {
    case MG_ARCH_UNIX: return "UNIX";
    case MG_ARCH_WIN32: return "WIN32";
    case MG_ARCH_ESP32: return "ESP32";
    case MG_ARCH_ESP8266: return "ESP8266";
    case MG_ARCH_FREERTOS: return "FREERTOS";
    case MG_ARCH_THREADX: return "THREADX";
    case MG_ARCH_ZEPHYR: return "ZEPHYR";
    case MG_ARCH_ARMGCC: return "ARMGCC";
    case MG_ARCH_CMSIS_RTOS1: return "CMSIS_RTOS1";
    case MG_ARCH_TIRTOS: return "TIRTOS";
    case MG_ARCH_PICOSDK: return "PICOSDK";
    case MG_ARCH_ARMCC: return "ARMCC";
    case MG_ARCH_CMSIS_RTOS2: return "CMSIS_RTOS2";
    case MG_ARCH_RTTHREAD: return "RTTHREAD";
    case MG_ARCH_ARMCGT: return "ARMCGT";
    case MG_ARCH_CUBE: return "CUBE";
    case MG_ARCH_CUSTOM: return "CUSTOM";
    default: return "unknown";
    }
}

static std::string jzMgInfo()
{
    std::string info = "MG_TLS=";
    info += std::to_string(MG_TLS);
    info += "(";
    info += jzMgTlsName();
    info += "), MG_ARCH=";
    info += std::to_string(MG_ARCH);
    info += "(";
    info += jzMgArchName();
    info += ")";
    return info;
}
#endif

const char *jzVersion()
{
    // 版本号 + 当前启用的模块列表 + TLS 后端 (参考 JZDriver.cpp 用 HAS_* 宏判断模块是否启用)
    static std::string s_version;
    if (!s_version.empty())
        return s_version.c_str();

    // 拼接启用的模块名
    std::vector<std::string> module_list;
    module_list.push_back("JZCore"); // JZCore 始终启用 (框架基础模块)
#ifdef HAS_JZJson
    module_list.push_back("JZJson");
#endif
#ifdef HAS_JZNet
    module_list.push_back("JZNet");
#endif
#ifdef HAS_JZFrp
    module_list.push_back("JZFrp");
#endif
#ifdef HAS_JZBle
    module_list.push_back("JZBle");
#endif
#ifdef HAS_JZWifi
    module_list.push_back("JZWifi");
#endif
#ifdef HAS_JZSerialPort
    module_list.push_back("JZSerialPort");
#endif
#ifdef HAS_JZGpio
    module_list.push_back("JZGpio");
#endif
#ifdef HAS_JZSensor
    module_list.push_back("JZSensor");
#endif
#ifdef HAS_JZCoroutine
    module_list.push_back("JZCoroutine");
#endif
#ifdef HAS_JZAgent
    module_list.push_back("JZAgent");
#endif
#ifdef HAS_JZJSEngine
    module_list.push_back("JZJSEngine");
#endif
#ifdef HAS_JZWidget
    module_list.push_back("JZWidget");
#endif
#ifdef HAS_JZSimulator
    module_list.push_back("JZSimulator");
#endif
#ifdef HAS_JZCamera
    module_list.push_back("JZCamera");
#endif
#ifdef HAS_JZCrashReport
    module_list.push_back("JZCrashReport");
#endif
#ifdef HAS_JZModbus
    module_list.push_back("JZModbus");
#endif
#ifdef HAS_JZOTA
    module_list.push_back("JZOTA");
#endif
    std::string modules = str_join(module_list, ",");

    std::string suffix;
#ifdef HAS_JZNet
    // MG_TLS / MG_ARCH 来自 mongoose.h, 仅在启用 JZNet 时打印
    suffix = ", " + jzMgInfo();
#endif

    // 版本信息改为展示 Git 信息 (参考 jz_version.h 中的 JZ_GIT_* 宏)
    std::string git_info = "branch:";
    git_info += JZ_GIT_BRANCH;
    git_info += ", commit:";
    git_info += JZ_GIT_COMMIT;
    if (JZ_GIT_DIRTY)
        git_info += "-dirty";

    s_version = git_info + " (modules: " + modules + suffix + ")";
    return s_version.c_str();
}