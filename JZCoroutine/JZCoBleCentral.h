#ifndef JZCO_BLE_CENTRAL_H_
#define JZCO_BLE_CENTRAL_H_

#include "JZCo.h"
#include "JZBLECentral.h"
#include "JZBluetoothDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include "JZResult.h"

#include <vector>

class JZCoBleService;

// JZBLECentral 的协程封装
// 在协程中调用 connectToDevice / discoverServices，异步结果通过 JZCoCondition 等待，返回 JZResult。
// 创建出的 JZCoBleService 由本对象管理生命周期（作为子对象），
// 与本对象同线程，保证信号直连、协程正确恢复（参考 JZCoTcpSocket）。
class JZCoBleCentral : public JZObject
{
public:
    JZCoBleCentral(JZObject *parent = nullptr);
    ~JZCoBleCentral() override;

    // 连接设备，超时返回错误
    JZResult<void> connectToDevice(const JZBluetoothDeviceInfo &info, int timeout = 30 * 1000);
    void disconnectFromDevice();

    // 发现服务，返回服务 uuid 列表
    JZResult<std::vector<JZBluetoothUuid>> discoverServices(int timeout = 30 * 1000);

    const JZBluetoothDeviceInfo &deviceInfo() const;

    // 创建协程版服务封装（生命周期由本对象管理）
    JZCoBleService *createService(const JZBluetoothUuid &uuid);

protected:
    enum WaitType
    {
        WaitNone,
        WaitConnect,
        WaitDiscover,
    };

    void connectSignals();
    void yield(WaitType type, int timeout);

    void onConnected();
    void onDisconnected();
    void onDiscoveryFinished();
    void onError(const std::string &msg);

    JZBLECentral *m_central;
    WaitType      m_waitType;
    JZCoCondition m_coCtrl;

    bool        m_connected;
    bool        m_discoveryFinished;
    bool        m_error;
    std::string m_errorMsg;
};

#endif // JZCO_BLE_CENTRAL_H_
