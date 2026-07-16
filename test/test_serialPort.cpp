#include "JZTest.h"
#include "JZSerialPort.h"
#include "JZDriver.h"
#include "JZApplication.h"
#include "JZSignal.h"
#include "JZDebug.h"

#include <chrono>
#include <thread>
#include <string>

// 纯逻辑自测：直接模拟驱动推送，验证 JZSerialPort 的缓冲与 sigReadyRead 通知。
// 不依赖真实串口硬件。
bool serial_push_receive()
{
    JZSerialPort sp("VIRTUAL");

    int readyCount = 0;
    connect(&sp.sigReadyRead, [&]() { readyCount++; });

    JZByteArray data("hello", 5);
    sp.onDataReceived(data);

    JZ_TEST_EQ(readyCount, 1);

    JZByteArray all = sp.readAll();
    JZ_TEST_EQ(all.size(), 5);
    JZ_TEST_EQ(std::string(all.data(), all.size()), std::string("hello"));

    // 再次推送多段，readAll 应拼接
    sp.onDataReceived(JZByteArray("AB", 2));
    sp.onDataReceived(JZByteArray("CD", 2));
    JZByteArray all2 = sp.readAll();
    JZ_TEST_EQ(all2.size(), 4);
    JZ_TEST_EQ(std::string(all2.data(), all2.size()), std::string("ABCD"));
    return true;
}

// 参数设置：未打开时仅保存配置，setX 应返回 true 且 getter 反映最新值。
bool serial_config()
{
    JZSerialPort sp("COM1");
    JZ_TEST_EQ(sp.baudRate(), 9600);

    JZ_TEST_TRUE(sp.setDataBits(JZSerialPort::Data7));
    JZ_TEST_EQ((int)sp.dataBits(), (int)JZSerialPort::Data7);

    JZ_TEST_TRUE(sp.setParity(JZSerialPort::EvenParity));
    JZ_TEST_EQ((int)sp.parity(), (int)JZSerialPort::EvenParity);

    JZ_TEST_TRUE(sp.setStopBits(JZSerialPort::TwoStop));
    JZ_TEST_EQ((int)sp.stopBits(), (int)JZSerialPort::TwoStop);

    JZ_TEST_TRUE(sp.setFlowControl(JZSerialPort::HardwareControl));
    JZ_TEST_EQ((int)sp.flowControl(), (int)JZSerialPort::HardwareControl);

    JZ_TEST_TRUE(sp.setBaudRate(115200));
    JZ_TEST_EQ(sp.baudRate(), 115200);
    return true;
}

// 关闭状态下收发应为空操作 / 空返回。
bool serial_closed_io()
{
    JZSerialPort sp("COM1");
    sp.write("abc", 3);
    JZByteArray r = sp.readAll();
    JZ_TEST_EQ(r.size(), 0);
    JZ_TEST_FALSE(sp.isOpen());
    return true;
}

// 打开不存在的端口应优雅失败。
bool serial_open_invalid()
{
    JZSerialPort sp("__NON_EXISTENT_PORT__");
    JZ_TEST_FALSE(sp.open());
    JZ_TEST_FALSE(sp.isOpen());
    return true;
}

// 枚举可用串口（依赖已注册的驱动单例）。
bool serial_available_ports()
{
    auto ports = JZSerialPort::availablePorts();
    jzDebug() << "serial_available_ports: 找到" << ports.size() << "个串口";
    for (auto &p : ports)
        jzDebug() << "  -" << p.portName << "(" << p.description << ")";
    // 仅验证调用不抛异常、返回容器可用，不强制数量
    return true;
}

// 真实回环测试：若系统存在可用串口，则打开第一个端口，写入后尝试读回。
// 需物理环回或虚拟串口对才能收到数据；无环回设备时仅验证打开/关闭链路。
bool serial_loopback()
{
    auto ports = JZSerialPort::availablePorts();
    if (ports.empty())
    {
        jzDebug() << "serial_loopback: 无可用串口，跳过回环测试";
        return true;
    }

    JZSerialPort sp(ports[0].portName);
    sp.setBaudRate(9600);
    sp.setDataBits(JZSerialPort::Data8);
    sp.setParity(JZSerialPort::NoParity);
    sp.setStopBits(JZSerialPort::OneStop);

    if (!sp.open())
    {
        jzDebug() << "serial_loopback: 打开" << ports[0].portName
                  << "失败（权限/被占用），跳过";
        return true;
    }
    JZ_TEST_TRUE(sp.isOpen());

    std::string sent = "JZSerialPort loopback test\n";
    sp.write(sent.data(), (int)sent.size());

    // 等待回环数据（最多 ~500ms）
    JZByteArray got;
    for (int i = 0; i < 50 && got.size() < (int)sent.size(); i++)
    {
        JZByteArray chunk = sp.readAll();
        if (!chunk.isEmpty())
            got.append(chunk.data(), chunk.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sp.close();
    JZ_TEST_FALSE(sp.isOpen());

    if (got.size() == (int)sent.size())
        JZ_TEST_EQ(std::string(got.data(), got.size()), sent);
    else
        jzDebug() << "serial_loopback: 未收到回环数据（请连接串口环回设备或虚拟串口对以验证收发）";

    return true;
}

// COM1<->COM2 回环测试：借助虚拟串口对（如 Windows 上的 com0com 等将 COM1 与 COM2
// 直连的 null-modem 配置），从一端写入、另一端读回，验证双向收发链路。
// 若两端端口任意一个无法打开（无对应环回对/被占用），则跳过而非报错。
bool serial_loopback_com1_com2()
{
    const char *kA = "COM1";
    const char *kB = "COM2";

    JZSerialPort a(kA);
    JZSerialPort b(kB);

    // 两端必须参数一致才能正常通信
    for (auto *sp : {&a, &b})
    {
        sp->setBaudRate(115200);
        sp->setDataBits(JZSerialPort::Data8);
        sp->setParity(JZSerialPort::NoParity);
        sp->setStopBits(JZSerialPort::OneStop);
        sp->setFlowControl(JZSerialPort::NoFlowControl);
    }

    if (!a.open() || !b.open())
    {
        jzDebug() << "serial_loopback_com1_com2: 无法打开 " << kA << "/" << kB
                  << "（请通过 com0com 等软件建立 COM1<->COM2 环回对后重试），跳过";
        if (a.isOpen()) a.close();
        if (b.isOpen()) b.close();
        return true;
    }
    JZ_TEST_TRUE(a.isOpen());
    JZ_TEST_TRUE(b.isOpen());

    // 小睡一下，确保两端驱动后台读线程都已完成首次打开
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto exchange = [](JZSerialPort &writer, JZSerialPort &reader,
                       const std::string &payload, const char *from, const char *to)
    {
        // 先清空可能残留的接收缓冲
        reader.readAll();

        writer.write(payload.data(), (int)payload.size());

        JZByteArray got;
        for (int i = 0; i < 100 && got.size() < (int)payload.size(); i++)
        {
            JZByteArray chunk = reader.readAll();
            if (!chunk.isEmpty())
                got.append(chunk.data(), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        bool ok = (got.size() == (int)payload.size())
                  && std::string(got.data(), got.size()) == payload;
        jzDebug() << "serial_loopback_com1_com2: " << from << "->" << to
                  << " 发送 " << payload.size() << "B, 收到 " << got.size()
                  << "B, " << (ok ? "OK" : "FAIL");
        return ok;
    };

    bool okAB = exchange(a, b, "COM1->COM2 hello world\n", kA, kB);
    bool okBA = exchange(b, a, "COM2->COM1 jzserial test\n", kB, kA);

    a.close();
    b.close();
    JZ_TEST_FALSE(a.isOpen());
    JZ_TEST_FALSE(b.isOpen());

    JZ_TEST_TRUE(okAB);
    JZ_TEST_TRUE(okBA);
    return true;
}

void test_JZSerialPort(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("JZSerialPort");
    test->addSuite(suite);
    suite->addTestCase("push_receive", serial_push_receive);
    suite->addTestCase("config", serial_config);
    suite->addTestCase("closed_io", serial_closed_io);
    suite->addTestCase("open_invalid", serial_open_invalid);
    suite->addTestCase("available_ports", serial_available_ports);
    suite->addTestCase("loopback", serial_loopback);
    suite->addTestCase("loopback_com1_com2", serial_loopback_com1_com2);
}

int main(int argc, char *argv[])
{
    JZApplication a;
    JZDriver::init();   // 注册平台串口驱动单例

    JZTest test;
    test_JZSerialPort(&test);
    test.run(argc, argv);

    JZDriver::deinit();
    return 0;
}
