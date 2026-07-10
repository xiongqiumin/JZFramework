#ifndef JZCO_BLE_DISCOVERY_AGENT_H_
#define JZCO_BLE_DISCOVERY_AGENT_H_

#include "JZCo.h"
#include "JZBLEDeviceDiscoveryAgent.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZResult.h"

#include <vector>

// JZBLEDeviceDiscoveryAgent 的协程封装
// 在协程中调用 scan()，异步扫描结果通过 JZCoCondition 等待，返回设备列表。
// 本对象及其底层 JZBLEDeviceDiscoveryAgent 创建在协程调度器线程上
// （参考 JZCoTcpSocket，将底层对象作为子对象即可保证同线程、直连信号）。
class JZCoBleDiscoveryAgent : public JZObject
{
public:
    JZCoBleDiscoveryAgent(JZObject *parent = nullptr);
    ~JZCoBleDiscoveryAgent() override;

    // 启动扫描，在 timeout 内收集所有发现的设备，返回设备列表
    JZResult<std::vector<JZBluetoothDeviceInfo>> scan(int timeout = 8000);

protected:
    enum WaitType
    {
        WaitNone,
        WaitScan,
    };

    void connectSignals();
    void yield(WaitType type, int timeout);

    void onDeviceDiscovered(const JZBluetoothDeviceInfo &info);
    void onFinished();
    void onError(int code, const std::string &msg);

    JZBLEDeviceDiscoveryAgent *m_agent;
    WaitType                   m_waitType;
    JZCoCondition              m_coCtrl;

    bool m_error;
    std::string m_errorMsg;
    std::vector<JZBluetoothDeviceInfo> m_devices;
};

#endif // JZCO_BLE_DISCOVERY_AGENT_H_
