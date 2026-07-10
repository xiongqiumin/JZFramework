#ifndef JZ_BLE_DRIVER_AC79_H_
#define JZ_BLE_DRIVER_AC79_H_

#include "JZBleDriver.h"
#include <map>
#include <vector>
#include <string>
#include <cstdint>

// 全局 C 回调(协议栈直接调用, 必须 C 链接) —— 前置声明, 供类内友元引用
extern "C" {
void ac79_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
// 以下两个回调由预编译 btstack.a 按固定 C 符号名直接调用, 名字不可改
void user_client_report_search_result(void *result_info);
void user_client_report_data_callback(void *report_data);
uint16_t ac79_att_read_cb(uint16_t con_handle, uint16_t att_handle, uint16_t offset,
                          uint8_t *buffer, uint16_t buffer_size);
int ac79_att_write_cb(uint16_t con_handle, uint16_t att_handle, uint16_t transaction_mode,
                      uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
}

class JZBleDriverAc79 : public JZBleDriver
{
public:
    JZBleDriverAc79();
    ~JZBleDriverAc79() override;

    bool isBluetoothAvailable() override;
    bool isBluetoothEnabled() override;

    // === Central (GATT Client) ===
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
    std::vector<JZLowEnergyCharacteristic> characteristics(
        const std::string& address, const std::string& serviceUuid) override;

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

    // === Peripheral (GATT Server) ===
    void addPeripheralCharacteristic(const std::string& svcUuid, const std::string& charUuid, int properties) override;
    void startAdvertising(const std::string& name, const std::vector<JZBluetoothUuid>& serviceUuids,
                          JZBLEPeripheral *d) override;
    void stopAdvertising() override;
    void peripheralSendNotify(const std::string& charUuid, const std::string& value) override;
    void peripheralSendIndicate(const std::string& charUuid, const std::string& value) override;

private:
    // 与框架(Esp32)保持一致的地址串格式: "xx:xx:xx:xx:xx:xx"(addr[5]..addr[0])
    static std::string addrToString(const uint8_t *addr);
    static void       addrFromString(const std::string &str, uint8_t *addrOut);
    int               getConnHandle(const std::string &a);

    // 框架 property 标志 <-> AC79 ATT_PROPERTY 标志
    static int jzPropToAtt(int jzProp);
    static int attPropToJz(int attProp);

    // JZBluetoothUuid -> 字节序列(is16 标记), 128-bit 采用小端存储(与 profile_data 一致)
    static void uuidToBytes(const JZBluetoothUuid &u, std::vector<uint8_t> &out, bool &is16);

    // 由已注册的 m_periphChars 动态构建 GATT 数据库
    std::vector<uint8_t> buildProfileData();

    // HCI/ATT 报文分发(中央与从机共用)
    void onPacket(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    // 扫描上报解析(从 adv_report_t 取设备名)
    void parseAdvName(const uint8_t *data, uint8_t len, std::string &name);

    // 中央角色上下文
    bool                                 m_scanning = false;
    std::map<std::string, int>           m_conHandleByAddr;   // addr -> conn handle
    std::map<int, std::string>           m_addrByConHandle;   // conn handle -> addr
    std::map<std::string, JZBLECentral*> m_centralByAddr;     // addr -> central
    JZBLECentral                        *m_svcDiscoverCtrl = nullptr;
    std::string                         m_svcDiscoverAddr;

    std::map<std::string, std::vector<JZBluetoothUuid>>            m_svcCache;  // addr -> services
    std::map<std::string, std::vector<JZLowEnergyCharacteristic>>  m_charCache;  // "addr|svc" -> chars
    // 记录每条特征的 value handle, 供读写时查找: "addr|svc" -> (charUuid -> valueHandle)
    std::map<std::string, std::map<std::string, uint16_t>>         m_charValHandle;

    // 读结果路由: valueHandle -> (addr, svc, char, servicePrivate)
    struct ReadCtx { std::string addr; std::string svc; std::string chr; JZBLEServicePrivate *d; };
    std::map<uint16_t, ReadCtx>        m_readCtx;
    // 通知/指示路由: valueHandle -> (addr, svc, servicePrivate)
    struct NtfCtx { std::string addr; std::string svc; JZBLEServicePrivate *d; };
    std::map<uint16_t, NtfCtx>         m_ntfCtx;

    // 从机角色上下文
    bool                             m_advertising = false;
    JZBLEPeripheral                 *m_periphCtrl = nullptr;
    int                              m_periphConnHandle = -1;
    struct PeriphChar { std::string svc; std::string chr; int properties; };
    std::vector<PeriphChar>          m_periphChars;                 // 已注册特征
    std::map<std::string, uint16_t>  m_periphValHandle;             // charUuid -> value handle
    std::map<uint16_t, std::string>  m_periphHandleChar;            // att handle -> charUuid
    std::map<uint16_t, bool>          m_periphCcc;                   // att handle -> 是否为 CCC
    std::map<uint16_t, std::string>  m_periphValueStore;            // value handle -> 当前值(供读回调)

    // 全局 C 回调(栈直接调用) —— 引用上方 extern "C" 前置声明
    friend void ac79_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    friend void user_client_report_search_result(void *result_info);
    friend void user_client_report_data_callback(void *report_data);
    friend uint16_t ac79_att_read_cb(uint16_t con_handle, uint16_t att_handle, uint16_t offset,
                                     uint8_t *buffer, uint16_t buffer_size);
    friend int ac79_att_write_cb(uint16_t con_handle, uint16_t att_handle, uint16_t transaction_mode,
                                 uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
};

#endif // !JZ_BLE_DRIVER_AC79_H_
