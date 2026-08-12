#ifndef JZCO_BLE_SERVICE_H_
#define JZCO_BLE_SERVICE_H_

#include "JZCo.h"
#include "JZBLEService.h"
#include "JZLowEnergyCharacteristic.h"
#include "JZLowEnergyDescriptor.h"
#include "JZBluetoothUuid.h"
#include "JZResult.h"
#include "JZByteArray.h"

#include <map>
#include <queue>
#include <vector>

// JZBLEService 的协程封装
// 所有接口在协程中调用，异步结果通过 JZCoCondition 等待，返回 JZResult。
// 注意：本对象及其底层 JZBLEService 必须创建在协程调度器线程上
//       （参考 JZCoTcpSocket，将底层对象作为子对象即可保证同线程、直连信号）。
class JZCoBleService : public JZObject
{
public:
    JZCoBleService(JZBLEService *service, JZObject *parent = nullptr);
    ~JZCoBleService() override;

    JZBluetoothUuid serviceUuid() const;

    // 发现特征值，返回特征值列表
    JZResult<std::vector<JZLowEnergyCharacteristic>> discoverDetails(int ms = -1);

    // 读取特征值，返回特征值内容
    JZResult<JZByteArray> readCharacteristic(const JZBluetoothUuid &charUuid, int ms = -1);

    // 写入特征值。writeNoResponse=true 时不等待写完成回调（fire and forget）
    JZResult<void> writeCharacteristic(const JZBluetoothUuid &charUuid, const JZByteArray &value,
                                       bool writeNoResponse = false, int ms = -1);

    JZResult<JZByteArray> readDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid, int ms = -1);
    JZResult<void> writeDescriptor(const JZBluetoothUuid &charUuid, const JZBluetoothUuid &descUuid,
                                   const JZByteArray &value, int ms = -1);

    // 等待一次通知/指示（需在已通过写 CCCD 描述符开启通知后调用）
    JZResult<JZByteArray> waitForChanged(const JZBluetoothUuid &charUuid, int ms = -1);

protected:
    enum WaitType
    {
        WaitNone,
        WaitDiscover,
        WaitRead,
        WaitWrite,
        WaitDescRead,
        WaitDescWrite,
        WaitChanged,
    };

    void connectSignals();
    void yield(WaitType type, int timeout);

    void onStateChanged(int state);
    void onCharacteristicRead(const JZLowEnergyCharacteristic &ch, const JZByteArray &value);
    void onCharacteristicWritten(const JZLowEnergyCharacteristic &ch, const JZByteArray &value);
    void onCharacteristicChanged(const JZLowEnergyCharacteristic &ch, const JZByteArray &value);
    void onDescriptorRead(const JZLowEnergyDescriptor &desc, const JZByteArray &value);
    void onDescriptorWritten(const JZLowEnergyDescriptor &desc, const JZByteArray &value);
    void onError(int code);

    JZBLEService *m_service;
    WaitType      m_waitType;
    JZCoCondition m_coCtrl;

    JZBluetoothUuid m_waitUuid;   // 当前等待的特征/描述符 uuid
    bool            m_error;
    std::string     m_errorMsg;

    bool            m_discoverDone;
    bool            m_hasResult;  // read/write 结果是否有效
    JZByteArray     m_resultValue;

    // 通知缓存（按特征 uuid 排队），供 waitForChanged 消费
    std::map<JZBluetoothUuid, std::queue<JZByteArray>> m_changedMap;
};

#endif // JZCO_BLE_SERVICE_H_
