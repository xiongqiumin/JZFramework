// JZJSModbusModule 端到端测试: modbus-serial 风格封装的 Modbus 主站(TCP)
//
// 架构(与 net/core/serialport/sqlite 一致):
//   JZJSModbusModule::initEngine 只注册 __jz.modbus 原子 API 与原生类 JZJSModbusClient;
//   js_wrapper/modbus/index.ts 经 tsc 编译为 modbus/dist/index.js, 把原子 API 包装成
//   modbus-serial 风格的 ModbusRTU / ModbusTCP, 由用户入口 import 被动加载。
//
// 覆盖: 适配层与原子 API 注入 / 未连接即读的拒绝 / un支持 connect* 的拒绝 /
//       连接事件 + isOpen / setID / FC3+FC6 往返 / FC1+FC5 往返 / FC16 多写 /
//       FC4 读输入寄存器 / FC2 读离散输入 / getInfo / close 事件与 isOpen。
//
// 回环: 本进程内起一个 JZModbusServer(127.0.0.1:port), JS 侧用 ModbusRTU.connectTCP
//       连上后做 ping-pong; 事件循环同时驱动 Modbus 服务端与 JS 引擎的 promise 泵。

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZJSEngine.h"
#include "JZJSModbusModule.h"
#include "JZModbusDefine.h"
#include "JZModbusServer.h"
#include "JZTimer.h"

using namespace std;

static int g_fail = 0;

static void check(const string& name, bool ok)
{
    fprintf(stderr, "[check] %s\n", name.c_str());
    fflush(stderr);
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

#define STEP(x) do { fprintf(stderr, "[step] %s\n", x); fflush(stderr); } while (0)

int test_jsmodbus(int argc, char *argv[])
{
    STEP("enter main");
    JZEventLoop loop;
    jzco_init();

    const int kPort = 15031;

    // ---- Modbus TCP 从站(与 test_modbus 的回环同款) ----
    JZModbusServer server;
    server.initMapping(100, 100, 100, 100);   // 100 线圈 / 100 离散输入 / 100 保持 / 100 输入
    server.setSlave(1);
    JZModbusConnetInfo sinfo;
    sinfo.modbusType = Modbus_tcpServer;
    sinfo.port = kPort;
    server.initConn(sinfo);
    check("modbus tcp server start", server.start());
    STEP("server start");

    // ---- JS 引擎 + modbus 模块 ----
    JZJSEngine engine;
    engine.init();
    // JS 适配层(modbus/dist/index.js)自己 `import { EventEmitter } from 'net/dist'` ——
    // 裸标识符要靠 library dir 才能解析到 js_wrapper 下(与 test_jsnet 的初始化一致)。
    engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });
    connect(&engine.sigLog, [](const string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string& s) { jzDebug() << "[js:error] " << s; });
    engine.start();
    STEP("engine start");

    JZJSModbusModule modbusModule;
    modbusModule.initEngine(&engine);
    STEP("modbus module init");

    // 测试侧引导: 加载 modbus 适配层并把导出挂成全局(生产侧是 import-only, 这里只为断言方便)
    {
        std::string wrapper = JZ_JS_WRAPPER_DIR;
        std::string tmp = wrapper + "/_test_modbus_globals.mjs";
        std::string mjs =
            "import ModbusRTU, { ModbusTCP } from './modbus/dist/index.js';\n"
            "globalThis.ModbusRTU = ModbusRTU; globalThis.ModbusTCP = ModbusTCP;\n"
            "globalThis.MODBUS_PORT = " + std::to_string(kPort) + ";\n";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f)
        {
            fwrite(mjs.data(), 1, mjs.size(), f);
            fclose(f);
        }
        engine.runModuleFile(tmp);
        remove(tmp.c_str());
    }
    STEP("modbus adapter injected");

    // 轮询等待某个 JS 表达式为真(同时泵事件循环 + 引擎的 promise 微任务, 让 TCP
    // 回调与 then/await 链落地)。纯 JS 的 then 链没有 C++ wakeUp 触发, 必须自己 execOnce。
    auto waitFor = [&](const string& expr, int timeoutMs) {
        bool done = false;
        JZTimer poll;
        connect(&poll.sigTimeout, [&]() {
            engine.execOnce();
            auto v = engine.eval(expr);
            if (!engine.isException(v.get()) && engine.fromJSValue<bool>(v.get()))
            {
                done = true;
                loop.quit();
            }
        });
        poll.start(50);

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&]() { loop.quit(); });
        wd.start(timeoutMs);

        loop.exec();
        poll.stop();
        wd.stop();
        return done;
    };

    auto evalScript = [&](const string& tag, const string& code) {
        auto v = engine.eval(code);
        if (engine.isException(v.get()))
            jzDebug() << "[eval exception]" << tag << engine.runtimeError();
    };

    auto runCheck = [&](const string& name, const string& expr) {
        auto v = engine.eval(expr);
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
        if (!ok && engine.isException(v.get()))
            jzDebug() << "[eval exception]" << name << engine.runtimeError();
        check(name, ok);
    };

    auto readGlobalStr = [&](const string& name) {
        auto v = engine.eval("globalThis." + name);
        return engine.isException(v.get()) ? string("<exception>")
                                           : engine.fromJSValue<string>(v.get());
    };

    // 0. 原子 API 与适配层注入
    runCheck("atomic api __jz.modbus", 
             "typeof __jz === 'object' && typeof __jz.modbus === 'object'"
             " && typeof __jz.modbus.create === 'function'"
             " && typeof __jz.modbus.open === 'function'"
             " && typeof __jz.modbus.read === 'function'"
             " && typeof __jz.modbus.write === 'function'"
             " && typeof __jz.modbus.close === 'function'"
             " && typeof __jz.modbus.isOpen === 'function'");
    runCheck("adapter injected (ModbusRTU/ModbusTCP)",
             "typeof ModbusRTU === 'function' && typeof ModbusTCP === 'function'"
             " && ModbusTCP === ModbusRTU && typeof globalThis.MODBUS_PORT === 'number'");

    // 1. 连接前: 读被拒绝 / getInfo() 为 null / setID 生效
    runCheck("client options before connect", 
             "(function(){ var c = new ModbusRTU();"
             " c.setID(7); c.setTimeout(3000); c.setPlcMode(true);"
             " return c.getID() === 7 && c.getTimeout() === 3000"
             "   && c.getInfo() === null && typeof c.getLastError() === 'string'"
             "   && c.isOpen === false; })()");

    evalScript("r1", 
        "globalThis.__r1 = 'pending';\n"
        "(function(){ var c = new ModbusRTU();\n"
        "  c.readHoldingRegisters(0, 1).then(\n"
        "     function(){ globalThis.__r1 = 'resolved'; },\n"
        "     function(e){ globalThis.__r1 = (String(e).indexOf('Port Not Open') >= 0) ? 'ok' : ('bad:' + String(e)); });\n"
        "})();\n");
    check("read before connect rejects", 
          waitFor("globalThis.__r1 !== 'pending'", 5000) && readGlobalStr("__r1") == "ok");
    if (readGlobalStr("__r1") != "ok")
        jzDebug() << "[info] __r1 =" << readGlobalStr("__r1");

    // 2. 不支持的连接方式一律 reject
    evalScript("r2",
        "globalThis.__r2 = 'pending';\n"
        "(function(){ var c = new ModbusRTU();\n"
        "  c.connectUDP().then(\n"
        "     function(){ globalThis.__r2 = 'resolved'; },\n"
        "     function(e){ globalThis.__r2 = (String(e).indexOf('not supported') >= 0) ? 'ok' : ('bad:' + String(e)); });\n"
        "})();\n");
    check("unsupported connectUDP rejects",
          waitFor("globalThis.__r2 !== 'pending'", 5000) && readGlobalStr("__r2") == "ok");
    if (readGlobalStr("__r2") != "ok")
        jzDebug() << "[info] __r2 =" << readGlobalStr("__r2");

    // 3. 主链路: connectTCP -> 读写往返 -> getInfo -> close
    evalScript("main", R"JS(
globalThis.__ok = 'pending';
globalThis.__log = '';
(async function () {
    try {
        globalThis.__log += 'new|';
        var c = new ModbusRTU();
        var evOpen = false, evClose = false;
        c.on('open', function () { evOpen = true; });
        c.on('close', function () { evClose = true; });

        globalThis.__log += 'connecting|';
        await c.connectTCP('127.0.0.1', { port: MODBUS_PORT });
        globalThis.__log += 'open=' + evOpen + ',isOpen=' + c.isOpen + '|';

        c.setID(1);
        globalThis.__log += 'id=' + c.getID() + '|';

        var w = await c.writeRegister(5, 0x1234);
        globalThis.__log += 'w=' + w.address + ':' + w.value + '|';
        var r = await c.readHoldingRegisters(5, 1);
        globalThis.__log += 'r=' + r.data[0] + '|';

        var wc = await c.writeCoil(3, true);
        globalThis.__log += 'wc=' + wc.address + ':' + wc.state + '|';
        var rc = await c.readCoils(3, 2);
        globalThis.__log += 'rc=' + rc.data.join(',') + '|';

        var wm = await c.writeRegisters(10, [1, 2, 3]);
        globalThis.__log += 'wm=' + wm.length + '|';
        var rm = await c.readHoldingRegisters(10, 3);
        globalThis.__log += 'rm=' + rm.data.join(',') + '|';

        var ri = await c.readInputRegisters(0, 2);
        globalThis.__log += 'ri=' + ri.data.join(',') + '|';
        var rd = await c.readDiscreteInputs(0, 2);
        globalThis.__log += 'rd=' + rd.data.join(',') + '|';

        var info = c.getInfo();
        globalThis.__log += 'info=' + info.port + '/' + info.slave + '/' + info.rtu + '|';

        c.close();
        globalThis.__log += 'closed=' + c.isOpen + ',evClose=' + evClose + '|';

        globalThis.__ok = 'ok';
    } catch (e) {
        globalThis.__log += 'EX:' + String(e);
        globalThis.__ok = 'err';
    }
})();
)JS");

    bool finished = waitFor("globalThis.__ok !== 'pending'", 20000);
    string log = readGlobalStr("__log");
    string okFlag = readGlobalStr("__ok");
    jzDebug() << "[modbus log]" << log;
    check("tcp roundtrip finished", finished && okFlag == "ok");
    if (okFlag != "ok")
        jzDebug() << "[info] __ok =" << okFlag;

    auto has = [&](const string& token) { return log.find(token) != string::npos; };
    check("connectTCP open event + isOpen", has("open=true,isOpen=true"));
    check("setID applied", has("id=1"));
    check("FC6 write + FC3 read roundtrip", has("w=5:4660") && has("r=4660"));
    check("FC5 write + FC1 read roundtrip", has("wc=3:true") && has("rc=true,false"));
    check("FC16 write multiple registers", has("wm=3") && has("rm=1,2,3"));
    check("FC4 read input registers", has("ri=0,0"));
    check("FC2 read discrete inputs", has("rd=false,false"));
    check("getInfo reports tcp", has("info=" + std::to_string(kPort) + "/1/false"));
    check("close emits 'close' + isOpen=false", has("closed=false,evClose=true"));

    // 4. 关闭后再读: 仍然拒绝(不崩)
    evalScript("r3",
        "globalThis.__r3 = 'pending';\n"
        "(function(){ var c = new ModbusRTU();\n"
        "  c.close();\n"
        "  c.readCoils(0, 1).then(\n"
        "     function(){ globalThis.__r3 = 'resolved'; },\n"
        "     function(e){ globalThis.__r3 = (String(e).indexOf('Port Not Open') >= 0) ? 'ok' : ('bad:' + String(e)); });\n"
        "})();\n");
    check("read after close rejects",
          waitFor("globalThis.__r3 !== 'pending'", 5000) && readGlobalStr("__r3") == "ok");
    if (readGlobalStr("__r3") != "ok")
        jzDebug() << "[info] __r3 =" << readGlobalStr("__r3");

    modbusModule.deinitEngine();
    server.stop();

    if (g_fail == 0)
        jzDebug() << "[test_jsmodbus] ALL PASS";
    else
        jzDebug() << "[test_jsmodbus] FAILED:" << g_fail;

    return g_fail == 0 ? 0 : 1;
}
