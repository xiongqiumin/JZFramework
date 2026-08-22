#ifndef JZ_OTA_DRIVER_SIMULATOR_H_
#define JZ_OTA_DRIVER_SIMULATOR_H_

#include "JZOTADriver.h"
#include "JZSimServer.h"
#include "JZObject.h"
#include "JZSimPack.h"
#include "JZSignal.h"

// OTA 模拟驱动(暂未实现)。仅作为 JZSimServer 平台下的占位实现,
// 保证 JZDriver::init 在 simulator 配置下可正常注册。功能待补充。
class JZOTADriverSimulator : public JZOTADriver, public JZObject
{
    JZ_OBJECT_DECLARE(JZOTADriverSimulator)
public:
    JZOTADriverSimulator();
    ~JZOTADriverSimulator() override;

    // 取当前 OTA 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZOTADriverSimulator *simInstance();

    // 每次本驱动与模拟器交互都会通过此信号广播。
    JZSignal<JZSimCmd, const JZVariantMap&> sigSim{this};

private:
    // 下行路由判断 / 下行处理(由注册进 JZSimServer 的 lambda 转发, 不再多继承)。
    bool support(JZSimCmd cmd) const;
    void onRecv(const JZSimPack &pack);

    JZSimServer *m_sim = nullptr;
    int m_simToken = 0;               // JZSimServer 注册令牌
    void sendSim(JZSimCmd cmd, const JZVariantMap &m);

    JZResult<void> verify(const JZOTAUpdateInfo &info) override;
    int64_t totalSize() override;
    bool begin(int64_t totalSize) override;
    int64_t write(int64_t offset, const JZByteArray &buffer) override;
    bool finish() override;
    void cancel() override;
    void reboot() override;
};

#endif // !JZ_OTA_DRIVER_SIMULATOR_H_
