#ifndef JZ_BLE_DRIVER_SIMULATOR_H_
#define JZ_BLE_DRIVER_SIMULATOR_H_

#include "JZBleDriver.h"
#include "JZByteArray.h"
#include "JZSimServer.h"
#include "JZSimPack.h"
#include "JZMutex.h"
#include <map>
#include <string>
#include <vector>

class JZNetPack;

// 基于 JZNetClient 的 BLE 驱动模拟实现。
// 内部把全部 BLE 操作封装成网络包发往 JZSimulatorDevice (运行 JZNetServer + LVGL 界面),
// 由后者模拟真实的 BLE 芯片行为并把结果回传。应用侧可像使用 Ac79/Esp32 驱动一样使用它。
class JZBleDriverSimulator : public JZBleDriver
{
public:
    JZBleDriverSimulator(const std::string &ip = "127.0.0.1", int port = 19123);
    ~JZBleDriverSimulator() override;

    // 取当前 BLE 驱动单例(仅当其为模拟驱动时返回, 否则 nullptr)
    static JZBleDriverSimulator *simInstance();

    // 由 JZSimServer 按模块段路由调用, 处理下行(BLE 段)网络包
    void onRecv(JZSimPack *pack);


    // === Central ===
    void startScan(JZBLEDeviceDiscoveryAgent *agent) override;
    void stopScan(JZBLEDeviceDiscoveryAgent *agent) override;
    bool isScanning(JZBLEDeviceDiscoveryAgent *agent) override;

    void connectToDevice(JZBLEController *d, const std::vector<JZBleBondKey>& keys) override;
    void disconnectFromDevice(JZBLEController *d) override;
    bool isConnected(JZBLEController *d) override;

    void discoverServices(const std::string& address, JZBLEController *d) override;
    std::vector<JZBluetoothUuid> services(const std::string& address) override;

    void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                 JZBLEService *d) override;
    std::vector<JZLowEnergyCharacteristic> characteristics(const std::string& address,
                                                          const std::string& serviceUuid) override;

    void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                            const std::string& charUuid, JZBLEService *d) override;
    void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, const JZByteArray& value,
                             bool writeNoResponse, JZBLEService *d) override;
    void enableNotification(const std::string& address, const std::string& serviceUuid,
                            const std::string& charUuid, bool enable, JZBLEService *d) override;
    void readDescriptor(const std::string& address, const std::string& serviceUuid,
                        const std::string& charUuid, const std::string& descUuid,
                        JZBLEService *d) override;
    void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                         const std::string& charUuid, const std::string& descUuid,
                         const JZByteArray& value, JZBLEService *d) override;

    // === Peripheral ===
    // 注：以下方法需与基类 JZBleDriver 的虚函数签名严格一致才能 override。
    // addPeripheralCharacteristic / peripheralSendNotify(value) / peripheralSendIndicate(value)
    // 是 Simulator 自有的辅助方法，不属于基类虚接口，故不带 override。
    void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties);
    void startAdvertising(JZBLEPeripheral *d) override;
    void stopAdvertising(JZBLEPeripheral *d) override;
    void peripheralSendNotify(const JZBluetoothUuid& charUuid, const JZByteArray& value);
    void peripheralSendIndicate(const JZBluetoothUuid& charUuid, const JZByteArray& value);
    void peripheralSendNotify(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect, const JZBluetoothUuid& charUuid, const JZByteArray& value) override;
    void peripheralDisconnect(JZBLEPeripheral *d, JZBLEPeripheralConnect *connect) override;

private:
    void sendSim(JZSimCmd cmd, const JZVariantMap &m);
    int  getConnHandle(const std::string &a);

    JZSimServer *m_sim = nullptr;
    bool m_scanning = false;
    JZObjectPointer m_scanAgent;

    // Central 状态
    std::map<std::string, int>            m_connMap;
    // 连接事件仅带 addr、无 object 指针，故只支持单个 Central（最近一次 connect 的）
    JZBLEController *m_central = nullptr;
    JZBLEController *m_svcDiscoverCtrl = nullptr;
    std::string      m_svcDiscoverAddr;
    JZBLEService *m_chrDiscoverCtrl = nullptr;
    std::string      m_chrDiscoverAddr;
    std::string      m_chrDiscoverSvc;

    std::map<std::string, std::vector<JZBluetoothUuid>>                  m_svcCache;
    std::map<std::string, std::vector<JZLowEnergyCharacteristic>>        m_charCache;
    std::map<std::string, std::map<std::string, int>>                    m_charValHandle; // addr|svc -> chr -> handle
    int m_handleCounter = 0;

    struct Ctx { std::string addr, svc, chr; JZByteArray value; JZBLEService *d = nullptr; };
    std::map<int, Ctx> m_readCtx;
    std::map<int, Ctx> m_ntfCtx;
    std::map<int, Ctx> m_writeCtx;

    // Peripheral 状态
    struct PeriphChar { std::string svc, chr; int props = 0; };
    std::vector<PeriphChar> m_periphChars;
    JZBLEPeripheral *m_periphCtrl = nullptr;
    std::shared_ptr<JZBLEPeripheralConnect> m_periphConnect;
    int m_periphConnHandle = -1;

    mutable JZMutex m_mutex;   // 保护所有跨线程共享状态（应用线程 <-> 网络接收线程）
};

#endif // !JZ_BLE_DRIVER_SIMULATOR_H_
