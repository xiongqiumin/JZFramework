#include "JZTest.h"
#include "JZModbusContext.h"
#include "JZModbusTcp.h"
#include "JZModbusDefine.h"
#include "JZModbusParam.h"
#include "JZModbusServer.h"
#include "JZModbusClient.h"
#include "JZEventLoop.h"
#include "JZApplication.h"
#include "net_test_common.h"

// 协议级自测: 直接构造 Modbus TCP 请求/响应, 验证 JZModbusContext 的编解码
// 不依赖真实网络或串口。流程: 构造请求 -> reply 生成响应 -> 解析响应。

// 读取保持寄存器: 构造请求, 经 mapping 生成响应, 再解析回数据
bool modbus_tcp_read_holding_registers()
{
    JZModbusContext ctx;
    initTcpContext(&ctx);
    ctx.setSlave(1);

    JZModbusMapping mapping;
    mapping.init(0, 0, 10, 0);   // 10 个保持寄存器, 起始地址 0
    mapping.tab_registers[0] = 0x1234;
    mapping.tab_registers[1] = 0x5678;
    mapping.tab_registers[2] = 0x9ABC;

    uint8_t req[260] = {0};
    uint8_t rsp[260] = {0};

    int req_len = ctx.buildRequestBasis(MODBUS_FC_READ_HOLDING_REGISTERS, 0, 3, req);
    ctx.sendMsgPre(req, req_len);

    MappingChanged chg;
    int rsp_len = ctx.reply(rsp, req, req_len, &mapping, chg);
    JZ_TEST_TRUE(rsp_len > 0);

    // 校验确认报文与请求匹配
    JZ_TEST_EQ(ctx.checkConfirmation(req, rsp, rsp_len), 3);

    // 解析响应
    JZByteArray res((const char *)rsp, rsp_len);
    JZ_TEST_EQ(ctx.receiveMessage(MSG_CONFIRMATION, res), rsp_len);

    std::vector<uint16_t> dest;
    ctx.readRegisters(res, 3, dest);
    JZ_TEST_EQ(dest.size(), 3u);
    JZ_TEST_EQ((int)dest[0], 0x1234);
    JZ_TEST_EQ((int)dest[1], 0x5678);
    JZ_TEST_EQ((int)dest[2], 0x9ABC);
    return true;
}

// 写单个保持寄存器: 请求写入一个值, mapping 中对应寄存器应被更新
bool modbus_tcp_write_single_register()
{
    JZModbusContext ctx;
    initTcpContext(&ctx);
    ctx.setSlave(1);

    JZModbusMapping mapping;
    mapping.init(0, 0, 10, 0);

    JZ_TEST_EQ((int)mapping.tab_registers[5], 0);

    uint8_t req[260] = {0};
    uint8_t rsp[260] = {0};

    int req_len = ctx.buildRequestBasis(MODBUS_FC_WRITE_SINGLE_REGISTER, 5, 0xABCD, req);
    ctx.sendMsgPre(req, req_len);

    MappingChanged chg;
    int rsp_len = ctx.reply(rsp, req, req_len, &mapping, chg);
    JZ_TEST_TRUE(rsp_len > 0);

    // 寄存器被写入
    JZ_TEST_EQ((int)mapping.tab_registers[5], 0xABCD);
    JZ_TEST_EQ(chg.addr, 5);
    JZ_TEST_EQ(chg.nb, 1);

    // 响应应回显请求
    JZ_TEST_EQ(ctx.checkConfirmation(req, rsp, rsp_len), 1);
    return true;
}

// 读线圈: 设置若干 bit, 经响应解析后逐位校验
bool modbus_tcp_read_coils()
{
    JZModbusContext ctx;
    initTcpContext(&ctx);
    ctx.setSlave(1);

    JZModbusMapping mapping;
    mapping.init(10, 0, 0, 0);   // 10 个线圈
    for (int i = 0; i < 10; i++)
        mapping.tab_bits[i] = (i % 2 == 0) ? 1 : 0;  // 0,2,4,6,8 = 1

    uint8_t req[260] = {0};
    uint8_t rsp[260] = {0};

    int req_len = ctx.buildRequestBasis(MODBUS_FC_READ_COILS, 0, 10, req);
    ctx.sendMsgPre(req, req_len);

    MappingChanged chg;
    int rsp_len = ctx.reply(rsp, req, req_len, &mapping, chg);
    JZ_TEST_TRUE(rsp_len > 0);

    JZByteArray res((const char *)rsp, rsp_len);
    JZ_TEST_EQ(ctx.receiveMessage(MSG_CONFIRMATION, res), rsp_len);

    // 响应中数据字节数 = (10/8)+1 = 2
    int byte_count = (10 / 8) + ((10 % 8) ? 1 : 0);
    std::vector<uint8_t> dest;
    ctx.readIoStatus(10, res, byte_count, dest);
    JZ_TEST_EQ(dest.size(), 10u);
    for (int i = 0; i < 10; i++)
        JZ_TEST_EQ((int)dest[i], (i % 2 == 0) ? 1 : 0);
    return true;
}

// 写单个线圈: 请求置位某线圈, mapping 中对应 bit 应更新
bool modbus_tcp_write_single_coil()
{
    JZModbusContext ctx;
    initTcpContext(&ctx);
    ctx.setSlave(1);

    JZModbusMapping mapping;
    mapping.init(10, 0, 0, 0);
    JZ_TEST_EQ((int)mapping.tab_bits[3], 0);

    uint8_t req[260] = {0};
    uint8_t rsp[260] = {0};

    int req_len = ctx.buildRequestBasis(MODBUS_FC_WRITE_SINGLE_COIL, 3, 0xFF00, req);
    ctx.sendMsgPre(req, req_len);

    MappingChanged chg;
    int rsp_len = ctx.reply(rsp, req, req_len, &mapping, chg);
    JZ_TEST_TRUE(rsp_len > 0);

    JZ_TEST_EQ((int)mapping.tab_bits[3], 1);
    JZ_TEST_EQ(chg.addr, 3);
    JZ_TEST_EQ(chg.nb, 1);
    return true;
}

// 非法地址应返回异常响应
bool modbus_tcp_illegal_address()
{
    JZModbusContext ctx;
    initTcpContext(&ctx);
    ctx.setSlave(1);

    JZModbusMapping mapping;
    mapping.init(0, 0, 4, 0);   // 仅 4 个保持寄存器

    uint8_t req[260] = {0};
    uint8_t rsp[260] = {0};

    // 读取地址 0 起 10 个寄存器 -> 超出范围
    int req_len = ctx.buildRequestBasis(MODBUS_FC_READ_HOLDING_REGISTERS, 0, 10, req);
    ctx.sendMsgPre(req, req_len);

    MappingChanged chg;
    int rsp_len = ctx.reply(rsp, req, req_len, &mapping, chg);
    JZ_TEST_TRUE(rsp_len > 0);

    // 异常响应: function 高位应被置 0x80
    JZ_TEST_EQ((int)rsp[7], MODBUS_FC_READ_HOLDING_REGISTERS + 0x80);
    JZ_TEST_EQ((int)rsp[8], MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS);
    return true;
}

// JZModbusParam pack/unpack 往返一致性(各数据类型)
bool modbus_param_pack_unpack()
{
    // uint16
    {
        JZModbusParam p;
        p.addrType = Param_HoldingRegister;
        p.dataType = JZModbusParam::Data_uint16;
        p.setValue(JZVariant::fromValue<uint16_t>(0x1234));

        std::vector<uint16_t> buf;
        p.pack(buf);
        JZ_TEST_EQ(buf.size(), 1u);

        JZModbusParam p2;
        p2.addrType = Param_HoldingRegister;
        p2.dataType = JZModbusParam::Data_uint16;
        p2.unpack(buf);
        JZ_TEST_EQ((int)p2.value.value<uint16_t>(), 0x1234);
    }
    // int32
    {
        JZModbusParam p;
        p.addrType = Param_HoldingRegister;
        p.dataType = JZModbusParam::Data_int32;
        p.setValue(JZVariant::fromValue<int32_t>(0x12345678));

        std::vector<uint16_t> buf;
        p.pack(buf);
        JZ_TEST_EQ(buf.size(), 2u);

        JZModbusParam p2;
        p2.addrType = Param_HoldingRegister;
        p2.dataType = JZModbusParam::Data_int32;
        p2.unpack(buf);
        JZ_TEST_EQ(p2.value.value<int32_t>(), int32_t(0x12345678));
    }
    // float
    {
        JZModbusParam p;
        p.addrType = Param_HoldingRegister;
        p.dataType = JZModbusParam::Data_float;
        p.setValue(JZVariant::fromValue<float>(3.14f));

        std::vector<uint16_t> buf;
        p.pack(buf);
        JZ_TEST_EQ(buf.size(), 2u);

        JZModbusParam p2;
        p2.addrType = Param_HoldingRegister;
        p2.dataType = JZModbusParam::Data_float;
        p2.unpack(buf);
        float v = p2.value.value<float>();
        JZ_TEST_TRUE(v > 3.13f && v < 3.15f);
    }
    return true;
}

// JZModbusParamMap 增删查
bool modbus_param_map()
{
    JZModbusParamMap map;

    JZModbusParam p;
    p.addr = 0;
        p.addrType = Param_HoldingRegister;
        p.dataType = JZModbusParam::Data_uint16;

        JZ_TEST_TRUE(map.add(p));
    JZ_TEST_EQ(map.count(), 1);
    JZ_TEST_TRUE(map.contains(0));
    JZ_TEST_TRUE(map.param(0) != nullptr);
    JZ_TEST_FALSE(map.contains(1));

    map.remove(0);
    JZ_TEST_EQ(map.count(), 0);
    JZ_TEST_FALSE(map.contains(0));
    JZ_TEST_TRUE(map.param(0) == nullptr);
    return true;
}

// ===================== Modbus TCP 回环测试 (参考 test/net) =====================
// 本地启动一个 JZModbusServer (127.0.0.1:port), 再用 JZModbusClient 连接,
// 以 写寄存器 -> 读回 -> 校验 的 ping-pong 方式验证真实 TCP 收发往返。
class ModbusLoopbackTest : public JZObject
{
public:
    enum { PhaseWrite, PhaseRead };

    ModbusLoopbackTest(JZEventLoop *loop, int port)
    {
        // Server: 100 个保持寄存器, 起始地址 0
        server.initMapping(0, 0, 100, 0);
        server.setSlave(1);

        JZModbusConnetInfo sinfo;
        sinfo.modbusType = Modbus_tcpServer;
        sinfo.port = port;
        server.initConn(sinfo);
        if (!server.start())
            netReportError("Modbus回环", "server start 失败");
        connect(&server.sigMappingRecved, this, &ModbusLoopbackTest::onServerRecved);

        client = new JZModbusClient(this);
        connect(&client->sigModbusOpen, this, &ModbusLoopbackTest::onOpen);
        connect(&client->sigModbusReplay, this, &ModbusLoopbackTest::onReplay);

        JZModbusConnetInfo cinfo;
        cinfo.modbusType = Modbus_tcpClient;
        cinfo.ip = "127.0.0.1";
        cinfo.port = port;
        cinfo.slave = 1;
        client->initConn(cinfo);
        client->openAsync();

        m_addr = 0;
        m_value = 0x1234;
        m_phase = PhaseWrite;

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== Modbus TCP 回环测试启动 (15s 后退出) ====";
    }

    void onOpen(bool ok)
    {
        if (!ok)
            netReportError("Modbus回环", "client 连接失败");
        writeNext();   // 启动 ping-pong
    }

    void onServerRecved(int addr, int nb)
    {
        // Server 侧确认收到本次写入, 地址/数量应一致
        if (addr != m_addr || nb != 1)
            netReportError("Modbus回环", "server 收到非法写入 addr=" + std::to_string(addr) + " nb=" + std::to_string(nb));
    }

    void writeNext()
    {
        m_phase = PhaseWrite;
        if (!client->writeRegisterAsync(m_addr, m_value))
            netReportError("Modbus回环", "writeRegisterAsync 失败");
    }

    void onReplay(const JZModebusReply &reply)
    {
        if (reply.code != 0)
            netReportError("Modbus回环", "reply 错误 code=" + std::to_string(reply.code));

        if (m_phase == PhaseWrite)
        {
            // 写入成功 -> 读回验证
            m_phase = PhaseRead;
            if (!client->readRegistersAsync(m_addr, 1))
                netReportError("Modbus回环", "readRegistersAsync 失败");
        }
        else
        {
            // 读回应答: 校验回环数据一致
            if (reply.reg.size() != 1)
                netReportError("Modbus回环", "读回寄存器数量异常 size=" + std::to_string(reply.reg.size()));
            if ((int)reply.reg[0] != m_value)
                netReportError("Modbus回环", "回环数据不一致 期望=" + std::to_string(m_value) + " 实际=" + std::to_string(reply.reg[0]));

            m_tps.hit(1);

            // 下一轮: 换地址/值, 维持 ping-pong
            m_addr = (m_addr + 1) % 100;
            m_value = (m_value + 0x1111) & 0xFFFF;
            writeNext();
        }
    }

    JZModbusServer server;
    JZModbusClient *client = nullptr;
    NetTpsCounter m_tps{"Modbus 回环"};

    int m_addr = 0;
    int m_value = 0;
    int m_phase = PhaseWrite;
};

int test_modbus_loopback()
{
    g_netTestActive = true;
    JZEventLoop loop;
    ModbusLoopbackTest test(&loop, 15021);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("Modbus 回环", tps, 20.0);
    g_netTestActive = false;
    return (int)tps;
}

void test_JZModbus(JZTest *test)
{
    auto suite = std::make_shared<JZTestSuite>("JZModbus");
    test->addSuite(suite);
    suite->addTestCase("tcp_read_holding_registers", modbus_tcp_read_holding_registers);
    suite->addTestCase("tcp_write_single_register", modbus_tcp_write_single_register);
    suite->addTestCase("tcp_read_coils", modbus_tcp_read_coils);
    suite->addTestCase("tcp_write_single_coil", modbus_tcp_write_single_coil);
    suite->addTestCase("tcp_illegal_address", modbus_tcp_illegal_address);
    suite->addTestCase("param_pack_unpack", modbus_param_pack_unpack);
    suite->addTestCase("param_map", modbus_param_map);
}

int main(int argc, char *argv[])
{
    JZApplication a;

    JZTest test;
    test_JZModbus(&test);
    test.run(argc, argv);

    jzDebug() << "########## Modbus TCP 回环测试 ##########";
    test_modbus_loopback();
    jzDebug() << "【OK】Modbus 回环测试通过";

    return 0;
}
