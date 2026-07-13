#ifndef JZ_BLE_DRIVER_SIMULATOR_H_
#define JZ_BLE_DRIVER_SIMULATOR_H_

#include "JZBleDriver.h"
#include "JZNetClient.h"
#include "JZSimPack.h"
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

    bool isBluetoothAvailable() override;
    bool isBluetoothEnabled() override;

    // === Central ===
    void startScan() override;
    void stopScan() override;
    bool isScanning() override;

    void connectToDevice(JZBLECentral *d) override;
    void disconnectFromDevice(JZBLECentral *d) override;
    bool isConnected(JZBLECentral *d) override;

    void discoverServices(const std::string& address, JZBLECentral *d) override;
    std::vector<JZBluetoothUuid> services(const std::string& address) override;

    void discoverCharacteristics(const std::string& address, const std::string& serviceUuid,
                                 JZBLEServicePrivate *d) override;
    std::vector<JZLowEnergyCharacteristic> characteristics(const std::string& address,
                                                          const std::string& serviceUuid) override;

    void readCharacteristic(const std::string& address, const std::string& serviceUuid,
                            const std::string& charUuid, JZBLEServicePrivate *d) override;
    void writeCharacteristic(const std::string& address, const std::string& serviceUuid,
                             const std::string& charUuid, const std::string& value,
                             bool writeNoResponse, JZBLEServicePrivate *d) override;
    void enableNotification(const std::string& address, const std::string& serviceUuid,
                            const std::string& charUuid, bool enable, JZBLEServicePrivate *d) override;
    void readDescriptor(const std::string& address, const std::string& serviceUuid,
                        const std::string& charUuid, const std::string& descUuid,
                        JZBLEServicePrivate *d) override;
    void writeDescriptor(const std::string& address, const std::string& serviceUuid,
                         const std::string& charUuid, const std::string& descUuid,
                         const std::string& value, JZBLEServicePrivate *d) override;

    // === Peripheral ===
    void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) override;
    void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                          JZBLEPeripheral *d) override;
    void stopAdvertising() override;
    void peripheralSendNotify(const std::string& charUuid, const std::string& value) override;
    void peripheralSendIndicate(const std::string& charUuid, const std::string& value) override;

private:
    void onPacket(JZNetPackPtr pack);
    void sendSim(JZSimCmd cmd, const JZVariantMap &m);
    int  getConnHandle(const std::string &a);

    JZNetClient *m_client = nullptr;
    bool m_scanning = false;

    // Central 状态
    std::map<std::string, int>            m_connMap;
    std::map<std::string, JZBLECentral*>  m_centralByAddr;
    JZBLECentral *m_svcDiscoverCtrl = nullptr;
    std::string      m_svcDiscoverAddr;
    JZBLEServicePrivate *m_chrDiscoverCtrl = nullptr;
    std::string      m_chrDiscoverAddr;
    std::string      m_chrDiscoverSvc;

    std::map<std::string, std::vector<JZBluetoothUuid>>                  m_svcCache;
    std::map<std::string, std::vector<JZLowEnergyCharacteristic>>        m_charCache;
    std::map<std::string, std::map<std::string, int>>                    m_charValHandle; // addr|svc -> chr -> handle
    int m_handleCounter = 0;

    struct Ctx { std::string addr, svc, chr, value; JZBLEServicePrivate *d = nullptr; };
    std::map<int, Ctx> m_readCtx;
    std::map<int, Ctx> m_ntfCtx;
    std::map<int, Ctx> m_writeCtx;

    // Peripheral 状态
    struct PeriphChar { std::string svc, chr; int props = 0; };
    std::vector<PeriphChar> m_periphChars;
    JZBLEPeripheral *m_periphCtrl = nullptr;
    int m_periphConnHandle = -1;
};

#endif // !JZ_BLE_DRIVER_SIMULATOR_H_
