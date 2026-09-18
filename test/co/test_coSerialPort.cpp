#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZDriver.h"
#include "JZCo.h"
#include "JZCoSerialPort.h"
#include "JZSerialPort.h"

#include "co_test_common.h"    // CO_TEST_ASSERT / coParseMsg

// 写端：从 COM1 依次发送 "serial <seq>\n"，小间隔让数据流入对端
static void serialWriter(JZCoSerialPort *port, int rounds)
{
    for (int i = 0; i < rounds; i++)
    {
        std::string msg = "serial " + std::to_string(i) + "\n";
        port->write(msg.data(), (int)msg.size());
        jzco_sleep(5);
    }
}

// 读端：从 COM2 逐行读回，校验每一条与发送一致（同时验证 readLine 接口）
static void serialReader(JZCoSerialPort *port, int rounds)
{
    for (int i = 0; i < rounds; i++)
    {
        auto r = port->readLine(-1, '\n');
        CO_TEST_ASSERT(r.isOk(), "serial readLine 超时/失败");
        std::string line = str_fromUtf8(r.value());
        uint64_t seq;
        CO_TEST_ASSERT(coParseMsg(line, "serial", seq) && seq == (uint64_t)i,
                       "serial echo 校验失败 期望=" + std::to_string(i) + " 收到=" + line);
    }
}

// ============ 协程串口: COM1<->COM2 回环 (ping-pong) ============
int test_coSerialPort()
{
    jzDebug() << "########## 协程串口测试 (COM1<->COM2 回环) ##########";
    const int rounds = 20;
    const char *kA = "COM1";
    const char *kB = "COM2";

    JZCoSerialPort a;
    JZCoSerialPort b;
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
        jzDebug() << "test_coSerialPort: 无法打开 " << kA << "/" << kB
                  << "（请用 com0com 等建立 COM1<->COM2 环回对），跳过";
        if (a.isOpen()) a.close();
        if (b.isOpen()) b.close();
        JZDriver::deinit();
        return 0;
    }
    jzDebug() << "test_coSerialPort: 已打开 " << kA << "/" << kB << "，开始 " << rounds << " 轮回环";

    auto writer = jzco_spawn([&]() { serialWriter(&a, rounds); });
    auto reader = jzco_spawn([&]() { serialReader(&b, rounds); });

    jzco_waitAll({writer, reader});

    a.close();
    b.close();
    JZDriver::deinit();
    jzDebug() << "【OK】协程串口 COM1<->COM2 回环通过 (" << rounds << " 轮)";
    return 0;
}
