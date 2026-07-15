#ifndef JZBLECONTROLLER_H
#define JZBLECONTROLLER_H

#include "JZObject.h"
#include "JZSignal.h"
#include "JZBLEDeviceInfo.h"
#include "JZBluetoothUuid.h"
#include <string>
#include <vector>

#include "JZBleDriver.h"
class JZBLEService;

class JZBLEController : public JZObject 
{
    JZ_OBJECT_DECLARE(JZBLEController)

    // 仅允许 BLE 驱动基类访问受保护的静态分发方法,
    // 具体驱动(如 JZBleDriverSimulator/Esp32/Ac79)统一通过 JZBleDriver 的转发中转调用。
    friend class JZBleDriver;
    
public:
    enum State { Unconnected, Connecting, Connected, Discovering };

    JZBLEController(JZObject *parent = nullptr);
    ~JZBLEController() override;

    const JZBLEDeviceInfo &deviceInfo() const;

    // 主机专属接口
    // 连接指定设备；keys 为该设备的绑定密钥（PEER_SEC / OUR_SEC，通常 1~2 份），
    // 连接前由驱动回灌以恢复加密通道。无密钥（首次配对）可不传。
    void connectToDevice(const JZBLEDeviceInfo &info,
                         const std::vector<JZBleDriver::JZBleBondKey> &keys = {});
    void disconnectFromDevice();

    void discoverServices();
    
    std::vector<JZBluetoothUuid> services() const;
    JZBLEService* createService(const JZBluetoothUuid &uuid);

jz_signals:
    JZSignal<> sigConnected;
    JZSignal<> sigDisconnected;
    JZSignal<> sigDiscoveryFinished;
    JZSigString sigError;
    JZSignal<State> sigStateChanged;
    // 配对完成后由驱动经 runOnObject 切到本线程 emit，参数为对端地址与绑定密钥。
    // 会分别上报 PEER_SEC(对端密钥) 与 OUR_SEC(本机密钥)，上层应两者都持久化，
    // 重连时密钥随 connectToDevice(keys) 传入并按 key.objType 各自注入，外设角色重连加密依赖 OUR_SEC。
    JZSignal<const std::string&, const JZBleDriver::JZBleBondKey&> sigBondKeyReported;

protected:
    JZBleDriver *getDriver();

    static void onDeviceConnected(JZBLEController *self, const char *addr);
    static void onDeviceDisconnected(JZBLEController *self, const char *addr);
    static void onServicesDiscovered(JZBLEController *self, const char *addr);
    static void onError(JZBLEController *self, int code, const char *msg);
    static void onBondKeyReported(JZBLEController *self, const std::string &addr, const JZBleDriver::JZBleBondKey &key);

    State                     m_state = Unconnected;
    std::string               m_errorMsg;
    JZBLEDeviceInfo     m_deviceInfo;
    std::vector<JZBluetoothUuid> m_serviceList;
};

#endif // JZBLECONTROLLER_H
