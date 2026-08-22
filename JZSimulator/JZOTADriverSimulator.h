#ifndef JZ_OTA_DRIVER_SIMULATOR_H_
#define JZ_OTA_DRIVER_SIMULATOR_H_

#include "JZOTADriver.h"
#include "JZObject.h"

// OTA 模拟驱动(暂未实现)。不注册到 JZSimServer 协议层(不占用 simulator 的 ota 类别路由),
// 仅作为 JZOTADriver 的占位实现, 保证 JZDriver::init 在 simulator 配置下可正常注册。
class JZOTADriverSimulator : public JZOTADriver, public JZObject
{
    JZ_OBJECT_DECLARE(JZOTADriverSimulator)
public:
    JZOTADriverSimulator();
    ~JZOTADriverSimulator() override;

    // 取当前 OTA 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZOTADriverSimulator *simInstance();

    JZResult<void> verify(const JZOTAUpdateInfo &info) override;
    int64_t totalSize() override;
    bool begin(int64_t totalSize) override;
    int64_t write(int64_t offset, const JZByteArray &buffer) override;
    bool finish() override;
    void cancel() override;
    void reboot() override;

    void markValid() override;
    bool isOTAReboot() override;
    void rollback() override;
};

#endif // !JZ_OTA_DRIVER_SIMULATOR_H_
