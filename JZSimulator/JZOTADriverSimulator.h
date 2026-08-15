#ifndef JZ_OTA_DRIVER_SIMULATOR_H_
#define JZ_OTA_DRIVER_SIMULATOR_H_

#include "JZOTADriver.h"
#include "JZSimPack.h"

// OTA 模拟驱动(暂未实现)。仅作为 JZSimulator 平台下的占位实现,
// 保证 JZDriver::init 在 simulator 配置下可正常注册。功能待补充。
class JZOTADriverSimulator : public JZOTADriver
{
public:
    JZOTADriverSimulator() = default;
    ~JZOTADriverSimulator() override = default;

    // 取当前 OTA 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZOTADriverSimulator *simInstance();

    // 由 JZSimServer 按模块段路由调用, 处理下行(OTA 段)网络包
    void onRecv(JZSimPack *pack);

    JZResult<void> verify(const JZOTAUpdateInfo &info) override;
    int64_t totalSize() override;
    bool begin(int64_t totalSize) override;
    int64_t write(int64_t offset, const JZByteArray &buffer) override;
    bool finish() override;
    void cancel() override;
    void reboot() override;
};

#endif // !JZ_OTA_DRIVER_SIMULATOR_H_
