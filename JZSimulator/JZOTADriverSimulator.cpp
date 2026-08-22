#include "JZOTADriverSimulator.h"
#include "JZApplication.h"
#include "JZDebug.h"
#include <fstream>

// reboot 标记文件名: 模拟驱动在 reboot 时写入 jzApp 所在目录,
// 进程重启后 isOTAReboot() 据此判断本次启动是否为升级后首次启动。
static const char *kOtaRebootFlagFile = "ota_reboot.txt";

// 取 jzApp 所在目录下的 reboot 标记文件完整路径。
static std::string otaRebootFlagPath()
{
    std::string dir = JZApplication::instance()->applicationDirPath();
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    return dir + kOtaRebootFlagFile;
}

JZ_OBJECT_IMPL(JZOTADriverSimulator, JZObject)

JZOTADriverSimulator *JZOTADriverSimulator::simInstance()
{
    return dynamic_cast<JZOTADriverSimulator *>(JZOTADriver::instance());
}

JZOTADriverSimulator::JZOTADriverSimulator()
{
    // OTA 模拟驱动不注册到 JZSimServer 协议层(不占用 simulator 的 ota 类别路由),
    // 仅作为 JZOTADriver 的占位实现, 保证 JZDriver::init 在 simulator 配置下可正常注册。
}

JZOTADriverSimulator::~JZOTADriverSimulator()
{
}

JZResult<void> JZOTADriverSimulator::verify(const JZOTAUpdateInfo &info)
{    
    return JZResult<void>::Ok();
}

int64_t JZOTADriverSimulator::totalSize()
{
    return -1;
}

bool JZOTADriverSimulator::begin(int64_t totalSize)
{
    return true;
}

int64_t JZOTADriverSimulator::write(int64_t offset, const JZByteArray &buffer)
{
    return buffer.size();
}

bool JZOTADriverSimulator::finish()
{
    return true;
}

void JZOTADriverSimulator::cancel()
{    
}

void JZOTADriverSimulator::reboot()
{
    // 模拟环境无真实硬件重启: 在 jzApp 所在目录写入标记文件,
    // 由外部(或测试)重新拉起进程后, isOTAReboot() 据此识别升级后首次启动。
    std::string path = otaRebootFlagPath();
    std::ofstream f(path, std::ios::trunc);
    if (f.is_open())
    {
        f << "reboot\n";
        jzDebug() << "JZOTADriverSimulator: wrote OTA reboot flag" << path;
        exit(0);
    }
    else
    {
        jzWarning() << "JZOTADriverSimulator: failed to write OTA reboot flag" << path;
    }
}

void JZOTADriverSimulator::markValid()
{
    // 新固件已确认可正常运行: 删除 reboot 标记文件, 表明升级观察期结束(不再回滚)。
    std::string path = otaRebootFlagPath();
    if (std::remove(path.c_str()) == 0)
        jzDebug() << "JZOTADriverSimulator: OTA reboot flag cleared" << path;
}

bool JZOTADriverSimulator::isOTAReboot()
{
    // 标记文件存在即视为升级后的首次启动(尚未 markValid)。
    std::ifstream f(otaRebootFlagPath());
    return f.is_open();
}

void JZOTADriverSimulator::rollback()
{
    // 主动回滚: 模拟环境无上一版本可回退, 仅清理标记文件并重置状态。
    std::string path = otaRebootFlagPath();
    std::remove(path.c_str());
    jzDebug() << "JZOTADriverSimulator: rollback (flag cleared)";
    exit(0);
}