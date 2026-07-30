/*
 * BLE Peripheral 上行流程测试 — 纯控制台
 *
 * 运行方式:
 *   1. 先启动 JZSimulator:  ./JZSimulator
 *   2. 再启动本测试:        ./test_ble_periph
 *   3. 在 JZSimulator 的 "BLE Central" 标签页中操作:
 *      a. 点击 "Connect to App Peripheral" → 本程序打印 [客户端已连接]
 *      b. 在特征行输入值, 点 "Write" → 本程序打印 [收到写入]
 *      c. 点击 "Disconnect" → 本程序打印 [客户端已断开], 自动重新广播
 */

#include <stdio.h>

#include "JZApplication.h"
#include "JZBleDriverSimulator.h"
#include "JZBLEPeripheral.h"
#include "JZBluetoothUuid.h"
#include "JZBleSimPack.h"
#include "JZSignal.h"
#include "JZDebug.h"
#include "JZTimer.h"
#include "JZByteArray.h"

// 自定义服务/特征 UUID
static JZBluetoothUuid SVC_UUID(static_cast<uint32_t>(0xFFF0));
static JZBluetoothUuid CHR_UUID(static_cast<uint32_t>(0xFFF1));

class BlePeriphTester : public JZObject
{
public:
    BlePeriphTester(JZObject *parent = nullptr)
        : JZObject(parent)
    {
        printf("=== BLE Peripheral Upstream Test ===\n");
        printf("Connecting to JZSimulator at 127.0.0.1:19123 ...\n");

        // 1. 创建模拟驱动
        m_driver = new JZBleDriverSimulator("127.0.0.1", 19123);

        // 2. 创建 Peripheral
        m_periph = new JZBLEPeripheral(this);
        connect(&m_periph->sigClientConnected,      [this]() { onConnected(); });
        connect(&m_periph->sigClientDisconnected,   [this]() { onDisconnected(); });
        connect(&m_periph->sigCharacteristicWritten, [this](const JZBluetoothUuid &uuid, const JZByteArray &val) {
            onCharWritten(uuid, val);
        });

        // 3. 注册服务 + 特征
        m_periph->addService(SVC_UUID);
        m_periph->addCharacteristic(SVC_UUID, CHR_UUID,
            CHC_Read | CHC_Write | CHC_WriteNoResponse | CHC_Notify,
            "hello");
        printf("[init] SVC=%s  CHR=%s  props=R/W/Notify\n",
               SVC_UUID.toString().c_str(), CHR_UUID.toString().c_str());

        // 4. 开始广播
        m_periph->startAdvertising("JZ_TestPeriph");
        printf("[init] startAdvertising(\"JZ_TestPeriph\") -> 等待 JZSimulator 连接...\n\n");

        // 5. 定时发送 Notify
        m_counter = 0;
        m_timer = new JZTimer(this);
        m_timer->setSingleShot(false);
        connect(&m_timer->sigTimeout, [this]() { onTimer(); });
        m_timer->start(3000);

        printf("提示: 切换到 JZSimulator -> BLE Central 标签进行操作\n");
        printf("----------------------------------------\n");
    }

private:
    void onConnected()
    {
        printf("[事件] 客户端已连接\n");
    }

    void onDisconnected()
    {
        printf("[事件] 客户端已断开 -> 重新广播\n");
        m_periph->startAdvertising("JZ_TestPeriph");
    }

    void onCharWritten(const JZBluetoothUuid &uuid, const JZByteArray &val)
    {
        printf("[事件] 收到写入: uuid=%s  value=\"%s\" (%d bytes)\n",
               uuid.toString().c_str(), str_fromUtf8(val).c_str(), val.size());
    }

    void onTimer()
    {
        if (!m_periph->isAdvertising()) return;

        m_counter++;
        char buf[64];
        snprintf(buf, sizeof(buf), "tick-%d", m_counter);
        JZByteArray val = str_toUtf8(buf);

        m_periph->sendNotify(CHR_UUID, val);
        printf("[通知] sendNotify: \"%s\"\n", str_fromUtf8(val).c_str());
    }

    JZBleDriverSimulator *m_driver = nullptr;
    JZBLEPeripheral       *m_periph = nullptr;
    JZTimer               *m_timer = nullptr;
    int                    m_counter = 0;
};

int main(int arg, char *argv[])
{
    JZApplication app;

    BlePeriphTester tester;

    return app.exec();
}
