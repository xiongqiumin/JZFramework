// BTE 协议层/传输层在 JS 引擎里的端到端测试
//
// 背景: binary/code_agent/runtime/bteserver 是把 JZBTEServer(Qt/C++ 上位机)逐行翻译成
//       JS 的产物。整体分三层:
//         (1) 协议层  protocol.js / structs.js   —— 纯 JS(Uint8Array + DataView), 本来就与 Qt 无关
//         (2) 传输层  netServer.js / netClient.js / netDataManager.js —— 原为 QTcpServer/QTcpSocket,
//                     已改到引擎的 net 模块(net/dist/index.js)
//         (3) 界面层  BTEServer.js 等             —— 依赖 Ui_BTEServerClass/QTableWidget/... 尚未收口
//       本测试只覆盖 (1)(2): 它们是"能在 JS 引擎里跑起来"的部分。
//
// 断言分四组:
//   1) 模块可加载  —— 含 main.js 载入即跑的 protocolSelfTest(失败会抛异常, 故"能加载"即"自测通过")
//   2) Qt 兼容层   —— qtcompat.js 的 Qt/QByteArray/QDataStream/QTimer/QSettings 等就位
//   3) stru_* 包类 —— 上报帧(0x7C/0x7D)与工步包骨架的 C++ 字段名往返
//   4) 传输层      —— NetServer/NetDataManager/NetPack 的组帧-解帧往返(纯内存, 不走 socket)
//
// 已知阻塞(2026-09-13, 非本模块引入): **引擎当前的真实 socket 路径会崩** ——
//   本测试曾用 NetServer.startServer(0) + 原始 socket 做 loopback 端到端,
//   每次都在第一次 listen() 处 0xC0000005; 同一时刻 test_jsnet.exe 也在它的
//   第 5 项(TCP echo, 第一次 listen/connect)同样崩, 属仓库级现象。
//   故第 4 组改为"不经 socket"的内存收发路径(它才是 bteserver 传输层真正的逻辑);
//   等 socket 栈修好后, 把 loopback 那几条断言加回来即可(见 git 历史/本条注释)。
//
// 需要: JZ_JS_WRAPPER_DIR(libraryDirs, 解析 'net/dist/index.js'/'core/dist/bytes.js')
//       JZ_BTESERVER_DIR(bteserver JS 源码目录)
//
// 注: 断言名一律用 ASCII —— jzDebug 的 [PASS]/[FAIL] 输出对中文会乱码, 影响定位。

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZEventLoop.h"
#include "JZJSCoreModule.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
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

int main()
{
    STEP("enter main");
    JZApplication a;
    JZEventLoop loop;
    jzco_init();

    JZJSEngine engine;
    engine.init();
    connect(&engine.sigLog, [](const string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string& s) { jzDebug() << "[js:error] " << s; });
    engine.start();
    // bteserver 的 protocol.js 用裸模块名 'core/dist/bytes.js', netServer.js 用 'net/dist/index.js'
    engine.setLibraryDir({ string(JZ_JS_WRAPPER_DIR) });
    STEP("engine start");

    JZJSNetModule netModule;
    netModule.initEngine(&engine);
    JZJSCoreModule coreModule;
    coreModule.initEngine(&engine);
    STEP("net/core module init");

    // 引导脚本: 以 ESM 加载 bteserver 的模块, 并把导出挂成全局供 eval 断言使用。
    // 注意 main.js 在**载入时**就跑 protocolSelfTest(), 任何用例失败都会抛异常 ->
    // 这里 runModuleFile 直接失败 -> 第 1 条断言即为"协议自测通过"。
    {
        const string dir = JZ_BTESERVER_DIR;   // 形如 F:/mcu/JZFramework/binary/code_agent/runtime/bteserver
        const string tmp = string(JZ_JS_WRAPPER_DIR) + "/_test_bteserver_globals.mjs";
        string mjs =
            "import * as mainMod from '" + dir + "/main.js';\n"
            "import { NetServer } from '" + dir + "/netServer.js';\n"
            "import { NetClient } from '" + dir + "/netClient.js';\n"
            "import { NetPackRegist, NetPackVariant, NETPACK_VARIANT } from '" + dir + "/netPackDefine.js';\n"
            "import { NetPackManager, PackInfo } from '" + dir + "/netPack.js';\n"
            "import { NetDataManager, blockingIoReport } from '" + dir + "/netDataManager.js';\n"
            "import * as B from '" + dir + "/structs.js';\n"
            "import * as P from '" + dir + "/protocol.js';\n"
            "import * as DT from '" + dir + "/datetime.js';\n"
            "import * as ST from '" + dir + "/storage.js';\n"
            "import { DataTrans } from '" + dir + "/DataTrans.js';\n"
            "import { BTEProject, DeviceData, SplitInfo } from '" + dir + "/BTEProject.js';\n"
            "import { BTERecordInfo, BTEStepInfo, BTERecord } from '" + dir + "/BTERecordDataBase.js';\n"
            "import { net } from 'net/dist/index.js';\n"
            "import qtCompat from '" + dir + "/qtcompat.js';\n"
            "globalThis.__bte = {\n"
            "  main: mainMod, B: B, P: P, DT: DT, ST: ST, DataTrans: DataTrans, net: net, qtCompat: qtCompat,\n"
            "  BTEProject: BTEProject, DeviceData: DeviceData, SplitInfo: SplitInfo,\n"
            "  BTERecordInfo: BTERecordInfo, BTEStepInfo: BTEStepInfo, BTERecord: BTERecord,\n"
            "  NetServer: NetServer, NetClient: NetClient, NetPackManager: NetPackManager,\n"
            "  NetDataManager: NetDataManager, blockingIoReport: blockingIoReport,\n"
            "  NetPackVariant: NetPackVariant, NETPACK_VARIANT: NETPACK_VARIANT,\n"
            "  CMD: P.CMD, encodeFrame: P.encodeFrame, hexOf: P.hexOf,\n"
            "};\n"
            // 放到最后: 注册失败也不影响上面的导出(便于定位真实原因)
            "NetPackRegist();\n";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f)
        {
            fwrite(mjs.data(), 1, mjs.size(), f);
            fclose(f);
        }
        bool loaded = engine.runModuleFile(tmp);
        remove(tmp.c_str());
        check("bteserver modules loaded (main.js runs protocolSelfTest on load)", loaded);
    }

    auto runCheck = [&](const string& name, const string& expr) {
        auto v = engine.eval(expr);
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
        check(name, ok);
    };

    // 求值为字符串(异常/不是字符串都返回 <..>, 便于失败时定位)
    auto evalStr = [&](const string& expr) -> string {
        auto v = engine.eval(expr);
        if (engine.isException(v.get()))
            return string("<exception>");
        if (!JS_IsString(v.get()))
            return string("<not-a-string>");
        return engine.fromJSValue<string>(v.get());
    };
    // 比较两个 JS 表达式求出的字符串, 不等时把两边都打出来
    auto checkEqJs = [&](const string& name, const string& a, const string& b) {
        string va = evalStr(a), vb = evalStr(b);
        bool ok = (va == vb);
        if (!ok)
            jzDebug() << "[eq-diff]" << name << "\n  A = " << va << "\n  B = " << vb;
        check(name, ok);
    };

    // 轮询等待某个 JS 表达式为真(同时泵 JS 任务队列, 让定时器/socket 回调落地)
    // 关键: 先 engine.execOnce() —— JS 的 setTimeout/setInterval 与引擎 socket 的事件派发
    // 都在引擎的 JS 任务队列里, 不泵就永远不触发。
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
        poll.start(20);

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&]() { loop.quit(); });
        wd.start(timeoutMs);

        loop.exec();
        poll.stop();
        wd.stop();
        return done;
    };

    if (g_fail > 0)
    {
        jzDebug() << "[test_bteserver] module load failed, skip rest";
        return 1;
    }

    // -----------------------------------------------------------------------
    // 2. Qt 兼容层
    // -----------------------------------------------------------------------
    runCheck("qtcompat: engine console has assert (Qt Q_ASSERT translation)",
             "typeof console.log === 'function' && typeof console.error === 'function'"
             " && typeof console.warn === 'function' && typeof console.assert === 'function'");

    checkEqJs("qtcompat: globals injected (Qt/QFile/QByteArray/QSettings/QColor/Ui_*)", R"JS(
(function () {
    var bad = [];
    function chk(name, want) {
        var got = typeof globalThis[name];
        if (got !== want) bad.push(name + '=' + got);
    }
    // 21 个类/构造器
    var cls = ['QFile', 'QDir', 'QTextStream', 'QSettings',
        'QLabel', 'QPushButton', 'QBrush', 'QFont', 'QFontMetrics',
        'QPoint', 'QRect', 'QColor', 'QPen', 'QThread',
        'QObject', 'QApplication', 'QMessageBox',
        'QTableWidget', 'QTableWidgetItem', 'QGridLayout', 'Ui_BTEServerClass'];
    for (var i = 0; i < cls.length; i++) chk(cls[i], 'function');
    // 两个对象: Qt 枚举表 + 兼容层本体(后者挂在 __bte 下, 不是全局)
    chk('Qt', 'object');
    if (typeof __bte.qtCompat !== 'object') bad.push('__bte.qtCompat');
    // 已换回 JS 原生的 Qt 类型不应再出现:
    //   定时器 -> setInterval/setTimeout; 时间 -> Date + ./datetime.js
    //   QByteArray/QDataStream -> 存档 JSON(./storage.js)
    chk('QTimer', 'undefined');
    chk('QDateTime', 'undefined');
    chk('QDate', 'undefined');
    chk('QTime', 'undefined');
    chk('QByteArray', 'undefined');
    chk('QDataStream', 'undefined');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    runCheck("qtcompat: Qt enum + named colors + qApp", R"JS(
(function () {
    return Qt.UserRole === 0x0100 && Qt.CustomContextMenu === 2
        && Qt.WindowMaximizeButtonHint === 0x02000000
        && (Qt.red instanceof QColor) && Qt.red.red() === 255
        && typeof qApp === 'object' && qApp instanceof QApplication
        && QApplication.applicationDirPath().length > 0;
})()
)JS");

    // 存档: 不再是 QByteArray+QDataStream 的二进制流, 改成 JSON(./storage.js)。
    // 约束是"存储格式自由、网络字节必须一致" —— 所以这里测 JSON 往返, 网络那部分由
    // 后面的 packData == encodeFrame / stru_* 逐字节断言保证。
    checkEqJs("storage: JSON 编解码往返(记录信息: Date/BigInt/嵌套/未设置)", R"JS(
(function () {
    var bad = [];
    var ST = __bte.ST, B = __bte.B;
    function eq(n, got, want) { if (got !== want) bad.push(n + ': ' + got + ' != ' + want); }

    var info = new __bte.BTERecordInfo();
    info.itemIndex = 3; info.recordId = 1007; info.recordNum = 12;
    info.recordTime = new Date(2026, 8, 13, 7, 5, 30);
    info.recordTimeEnd = new Date(2026, 8, 13, 9, 30, 0);
    info.stepInfo.startStepIndex = 1;
    info.stepInfo.creator = 'tester';
    info.stepInfo.cellNo = 'CELL-1';
    info.stepInfo.step.stepList.push(new B.stru_BTS_CMD_SET_STEP76_Ex.StepInfo());
    info.stepInfo.step.stepList[0].StepType = 2;
    info.stepInfo.step.stepList[0].paramList = [11, 22.5, 33];
    info.rtData.Tempature = 253; info.rtData.Voltage = 40500;
    info.rtData.ChargeCap = 1800000000n;          // int64 -> BigInt
    info.dataCache.ccRatio = 88.5; info.dataCache.beginDis = 4;

    // 走一遍真正的"序列化 -> 文本 -> 反序列化"
    var text = JSON.stringify(ST.recordInfoToJson(info));
    var back = new __bte.BTERecordInfo();
    ST.recordInfoSetJson(back, JSON.parse(text));

    eq('itemIndex', back.itemIndex, 3);
    eq('recordId', back.recordId, 1007);
    eq('recordNum', back.recordNum, 12);
    eq('recordTime', ST.dateToJson(back.recordTime), ST.dateToJson(info.recordTime));
    eq('recordTimeEnd', ST.dateToJson(back.recordTimeEnd), ST.dateToJson(info.recordTimeEnd));
    eq('stepInfo.creator', back.stepInfo.creator, 'tester');
    eq('stepInfo.cellNo', back.stepInfo.cellNo, 'CELL-1');
    eq('stepList.len', back.stepInfo.step.stepList.length, 1);
    eq('stepType', back.stepInfo.step.stepList[0].StepType, 2);
    eq('paramList', back.stepInfo.step.stepList[0].paramList.join(','), '11,22.5,33');
    eq('rtData.Tempature', back.rtData.Tempature, 253);
    eq('rtData.ChargeCap(BigInt)', String(back.rtData.ChargeCap), '1800000000');
    eq('ccRatio', back.dataCache.ccRatio, 88.5);
    eq('beginDis', back.dataCache.beginDis, 4);

    // 「未设置」的 Date 必须是 null(不能变成"现在")
    var empty = new __bte.BTERecordInfo();
    eq('默认未设置', String(ST.dateToJson(empty.recordTime)), 'null');
    eq('null 往返', String(ST.dateFromJson(null)), 'null');
    eq('Uint8Array 往返', ST.u8ToArray(ST.arrayToU8([1, 2, 3], 4)).join(','), '1,2,3,0');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // 工程存档: 真正落盘 -> 读回来(.prj 现在是 JSON)
    {
        JZDir tmpDir(JZDir::currentPath() + "/bte_storage_tmp");
        if (!tmpDir.exists())
            tmpDir.mkpath(tmpDir.path());
        engine.eval("globalThis.__tmpDir = '" + tmpDir.path() + "';");
    }
    checkEqJs("storage: BTEProject save/load 往返(.prj JSON)", R"JS(
(function () {
    var bad = [];
    function eq(n, got, want) { if (got !== want) bad.push(n + ': ' + got + ' != ' + want); }
    QApplication.setApplicationDirPath(__tmpDir);

    var p1 = new __bte.BTEProject();
    p1.init();

    var dev = new __bte.DeviceData();
    dev.deviceId = '10.0.0.1';
    dev.deviceInfo.VoltageMax = 5000;
    dev.deviceInfo.DeviceString = '5V100A';
    dev.stepList[0] = new __bte.BTEStepInfo();
    dev.stepList[0].startStepIndex = 1;
    dev.stepList[0].creator = 'tester';
    dev.stepList[0].cellNo = 'CELL-1';
    dev.recordList[0].info.itemIndex = 0;
    dev.recordList[0].info.recordId = 1000;
    dev.recordList[0].info.recordTime = new Date(2026, 8, 13, 7, 5, 30);
    dev.recordList[0].info.recordNum = 3;
    dev.recordList[0].info.rtData.Tempature = 253;
    dev.parallelList.push({ channels: [0, 1, 2] });
    dev.enableItem = 8;
    p1.m_devices.set(dev.deviceId, dev);

    var sp = new __bte.SplitInfo();
    sp.name = 'A';
    sp.rangeList[0][0].vaild = true;
    sp.rangeList[0][0].lower = 1.5;
    sp.rangeList[0][0].upper = 2.5;
    p1.m_splitInfoList.push(sp);
    p1.save();                                   // 落盘(顺带走一遍 recordDataBase 备份路径)

    var p2 = new __bte.BTEProject();
    p2.init();
    p2.load();                                   // 读回来(记录库不可用时也不该抛)

    eq('devices', p2.m_devices.size, 1);
    var d2 = p2.m_devices.get('10.0.0.1');
    eq('deviceId', d2 ? d2.deviceId : null, '10.0.0.1');
    eq('VoltageMax', d2 ? d2.deviceInfo.VoltageMax : -1, 5000);
    eq('DeviceString', d2 ? d2.deviceInfo.DeviceString : null, '5V100A');
    eq('enableItem', d2 ? d2.enableItem : -1, 8);
    eq('parallel.channels', d2 ? d2.parallelList[0].channels.join(',') : null, '0,1,2');
    eq('stepList[0].creator', (d2 && d2.stepList[0]) ? d2.stepList[0].creator : null, 'tester');
    eq('stepList[0].cellNo', (d2 && d2.stepList[0]) ? d2.stepList[0].cellNo : null, 'CELL-1');
    eq('recordInfo.recordId', d2 ? d2.recordList[0].info.recordId : -1, 1000);
    eq('recordInfo.recordNum', d2 ? d2.recordList[0].info.recordNum : -1, 3);
    eq('recordInfo.rtData.Tempature', d2 ? d2.recordList[0].info.rtData.Tempature : -1, 253);
    eq('recordTime', d2 ? __bte.DT.formatDateTime(d2.recordList[0].info.recordTime, 'yyyy-MM-dd HH:mm:ss') : null,
       '2026-09-13 07:05:30');
    eq('recordTimeEnd(未设置)', String(d2 ? d2.recordList[0].info.recordTimeEnd : 'x'), 'null');
    eq('splitInfoList', p2.m_splitInfoList.length, 1);
    eq('split.name', p2.m_splitInfoList[0].name, 'A');
    eq('split.upper', p2.m_splitInfoList[0].rangeList[0][0].upper, 2.5);
    eq('split.vaild', p2.m_splitInfoList[0].rangeList[0][0].vaild, true);
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // 定时器一律走 JS 原生 setInterval/setTimeout —— qtcompat 不再提供 QTimer(2026-09-13)。
    runCheck("timer: QTimer shim removed, JS native timers available", R"JS(
(function () {
    return typeof QTimer === 'undefined'
        && typeof setInterval === 'function' && typeof clearInterval === 'function'
        && typeof setTimeout === 'function' && typeof clearTimeout === 'function';
})()
)JS");

    engine.eval(R"JS(
(function () {
    globalThis.__js_ticks = 0;
    globalThis.__js_timer = setInterval(function () { __js_ticks++; }, 10);
})()
)JS");
    check("timer: JS setInterval actually fires (BTEServer 的三个定时器靠它)",
          waitFor("typeof __js_ticks !== 'undefined' && __js_ticks > 0", 2000));
    engine.eval("clearInterval(__js_timer); globalThis.__js_timer = 0;");

    // 时间: Date + datetime.js 取代 QDateTime/QDate/QTime(2026-09-13)
    checkEqJs("datetime: formatDateTime 支持 Qt 格式串(yyyy/yy/MM/dd/HH/hh/mm/ss/zzz + [..] 字面量)", R"JS(
(function () {
    var bad = [];
    function eq(name, got, want) { if (got !== want) bad.push(name + ': ' + got + ' != ' + want); }
    var d = new Date(2026, 8, 13, 7, 5, 30, 45);   // 2026-09-13 07:05:30.045
    eq('yyyy-MM-dd HH:mm:ss', __bte.DT.formatDateTime(d, 'yyyy-MM-dd HH:mm:ss'), '2026-09-13 07:05:30');
    eq('yyMMddhhmmss', __bte.DT.formatDateTime(d, 'yyMMddhhmmss'), '260913070530');
    eq('[HH:mm:ss:zzz] ', __bte.DT.formatDateTime(d, '[HH:mm:ss:zzz] '), '[07:05:30:045] ');
    eq('yyyyMMdd_hhmmss', __bte.DT.formatDateTime(d, 'yyyyMMdd_hhmmss'), '20260913_070530');
    eq('默认格式', __bte.DT.formatDateTime(d), '2026-09-13 07:05:30');
    eq('null -> ""', __bte.DT.formatDateTime(null, 'yyyy'), '');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    checkEqJs("datetime: secsTo/daysTo/lt/gt 的 Qt 语义(无效时间返回 0/false)", R"JS(
(function () {
    var bad = [];
    function eq(name, got, want) { if (got !== want) bad.push(name + ': ' + got + ' != ' + want); }
    var d = new Date(2026, 8, 13, 7, 5, 30);
    var later = new Date(d.getTime() + 5000);
    var D = __bte.DT;

    // secsTo: 无效操作数 -> 0 (业务靠它表达"还没发过包/还没心跳就不要超时")
    eq('secondsBetween', String(D.secondsBetween(d, later)), '5');
    eq('secondsBetween(null)', String(D.secondsBetween(null, later)), '0');
    eq('secondsBetween(逆序)', String(D.secondsBetween(later, d)), '-5');
    // daysTo: 按日历天(23:00 -> 次日 01:00 = 1), 与 QDate::daysTo 一致
    eq('daysBetween', String(D.daysBetween(d, new Date(2026, 8, 15, 0, 0, 0))), '2');
    eq('daysBetween(跨零点)', String(D.daysBetween(new Date(2026, 8, 13, 23, 0, 0), new Date(2026, 8, 14, 1, 0, 0))), '1');
    eq('daysBetween(null)', String(D.daysBetween(null, later)), '0');
    // lt/gt: null 一律 false —— 不能落到 JS 的 `null < date`(null 转 0, 结果恒为 true)
    eq('isBefore(null)', String(D.isBefore(null, later)), 'false');
    eq('isAfter(null)', String(D.isAfter(null, later)), 'false');
    eq('isBefore', String(D.isBefore(d, later)), 'true');
    eq('isAfter', String(D.isAfter(d, later)), 'false');
    // addSecs/addDays/asDate
    eq('addSeconds', String(D.addSeconds(d, 60).getMinutes()), '6');
    eq('addDays', String(D.addDays(d, 1).getDate()), '14');
    eq('asDate(非 Date)', String(D.asDate({})), 'null');
    eq('asDate(null)', String(D.asDate(null)), 'null');
    eq('asDate(Date)', String(D.asDate(d) === d), 'true');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    checkEqJs("DataTrans: 时长换算不再用 QTime(纯毫秒算术, 往返一致)", R"JS(
(function () {
    var D = __bte.DataTrans;
    var ms = 86400000 + 3600000 + 120000 + 3000 + 4;   // 1 天 + 01:02:03.004 => 25:02:03:004
    var bad = [];
    if (D.fromTimeUnit(ms) !== '25:02:03:004') bad.push('fromTimeUnit=' + D.fromTimeUnit(ms));
    if (D.fromTimeUnitSec(ms) !== '25:02:03') bad.push('fromTimeUnitSec=' + D.fromTimeUnitSec(ms));
    if (D.toTimeUnit('25:02:03:004') !== ms) bad.push('toTimeUnit=' + D.toTimeUnit('25:02:03:004'));
    if (D.toTimeUnit(D.fromTimeUnit(ms)) !== ms) bad.push('roundtrip');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // -----------------------------------------------------------------------
    // 3. stru_* 包类(界面/数据层用的 C++ 字段名接口)
    // -----------------------------------------------------------------------
    checkEqJs("stru_*: RTDATA76 (0x7C) round trip + workStatus", R"JS(
(function () {
    var o = {
        dfDataPointer: 1234, rtDataCount: 1, dataFlag: 0x55, channelIdByNum: 3,
        auxChlIdByNum: 0, cycleStatus: 2, stepId: 5, stepType: 1, workType: 0x10,
        testTime: 123456789, voltage: 40500, current: 250000, internalResistance: 1200,
        tempature: 253, chargeCap: 1800000000, disChargeCap: 0, chargeEng: 0,
        disChargeEng: 0, curStepRange: 3000, prtCode: 0,
    };
    var RT = __bte.B.stru_BTS_CMD_SEND_RTDATA76;
    var bad = [];
    var rt = new RT();
    if (!rt.unpack(__bte.B.packRTData76(o))) bad.push('unpack=false');
    if (rt.ChannelIDByNum !== 3) bad.push('ChannelIDByNum=' + rt.ChannelIDByNum);
    if (rt.StepID !== 5) bad.push('StepID=' + rt.StepID);
    if (rt.PrtCode !== 0) bad.push('PrtCode=' + rt.PrtCode);
    if (rt.workStatus() !== RT.WORK_RUN) bad.push('workStatus=' + rt.workStatus());
    if (rt.TestTime.toString() !== '123456789') bad.push('TestTime=' + rt.TestTime);
    if (rt.ChargeCap.toString() !== '1800000000') bad.push('ChargeCap=' + rt.ChargeCap);
    if (rt.Tempature !== 253) bad.push('Tempature=' + rt.Tempature);
    if (rt.Voltage !== 40500) bad.push('Voltage=' + rt.Voltage);
    if (rt.WorkType !== 0x10) bad.push('WorkType=' + rt.WorkType);

    // fault frame: WorkType bit5..4 = 11 (safe stop) + bit7 (fault) + PrtCode != 0
    var f = new RT();
    f.unpack(__bte.B.packRTData76(Object.assign({}, o, { workType: 0xB0, prtCode: 0x12 })));
    if (f.workStatus() !== RT.WORK_ERROR_STOP) bad.push('fault.workStatus=' + f.workStatus());
    if (f.PrtCode !== 0x12) bad.push('fault.PrtCode=' + f.PrtCode);
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    runCheck("stru_*: DFDATA76 (0x7D) round trip (ATime + nested Data)", R"JS(
(function () {
    var body = __bte.B.packDFData76({
        dfDataPointer: 88,
        dataList: [
            { dataFlag: 0x59, subDevId: 1, channelIdByNum: 3, auxChlIdByNum: 0, testId: 4200,
              dataSerial: 1, cycleStatus: 0, stepId: 1, stepType: 1, stepChangeCount: 0,
              workType: 0x10, testTime: 1000, voltage: 41230, current: 250000,
              internalResistance: 0, tempature: 250, chargeCap: 100, disChargeCap: 0,
              chargeEng: 0, disChargeEng: 0, curStepRange: 3000,
              aTime: { year: 2026, month: 9, day: 13, hour: 7, minute: 5, second: 30 } },
            { dataFlag: 0x55, subDevId: 1, channelIdByNum: 3, auxChlIdByNum: 0, testId: 4200,
              dataSerial: 7, cycleStatus: 0, stepId: 2, stepType: 2, stepChangeCount: 1,
              workType: 0x10, testTime: 2000, voltage: 39800, current: -250000,
              internalResistance: 1100, tempature: 260, chargeCap: 200, disChargeCap: 50,
              chargeEng: 0, disChargeEng: 0, curStepRange: 3000,
              aTime: { year: 2026, month: 9, day: 13, hour: 30, minute: 5, second: 40 } },
        ],
    });
    var df = new __bte.B.stru_BTS_CMD_SEND_DFDATA76();
    var ok = df.unpack(body);
    ok = ok && df.DFDataNumber === 2 && df.DFDataPointer === 88;
    ok = ok && df.dataList.length === 2;
    ok = ok && df.dataList[0].TestID === 4200 && df.dataList[0].Voltage === 41230;
    ok = ok && df.dataList[1].Current === -250000;
    ok = ok && __bte.B.aTimeToISO(df.dataList[1].ATime) === '2026-09-14 06:05:40';
    // nested Data single-frame round trip
    var d = new __bte.B.stru_BTS_CMD_SEND_DFDATA76.Data();
    ok = ok && d.unpack(body.slice(5, 5 + 83)) && d.ChannelIDByNum === 3;
    return ok;
})()
)JS");

    runCheck("stru_*: step-pack skeleton (ParamInfo/typeList/STEP_REQUEST bitmap)", R"JS(
(function () {
    var SE = __bte.B.stru_BTS_CMD_SET_STEP76_Ex;
    var ok = (SE.StepInfo.ParamInfo.TYPE_PARAM === 2)
        && (SE.StepInfo.ParamInfo.TYPE_NONE === 0)
        && (SE.StepInfo.typeList.length === 64);
    var req = new __bte.B.stru_BTS_CMD_STEP_REQUEST();
    req.setChannel([0, 3, 9]);
    ok = ok && req.channels().join(',') === '0,3,9';
    ok = ok && req.type() === __bte.CMD.SET_STEP76_Ex;
    // 0x0200 bitmap/param table not ported yet -> must fail loudly, not return wrong data
    var step = new SE.StepInfo();
    ok = ok && (step.unpack(new Uint8Array(8)) === false);
    return ok;
})()
)JS");

    checkEqJs("stru_*: control frames match protocol.js byte for byte", R"JS(
(function () {
    var bad = [];
    function step(name, fn) {
        try {
            var got = fn();
            if (got !== null) bad.push(name + ' -> ' + got);
        } catch (e) { bad.push(name + ' EXC: ' + e); }
    }

    step('START70', function () {
        var s = new __bte.B.stru_BTS_CMD_START70();
        s.Guid = new Uint8Array([0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]);
        s.StepID = 1;
        s.ChannelID = __bte.P.channelMask([0, 1]);
        var a = __bte.hexOf(s.pack());
        var b = __bte.hexOf(__bte.B.packStart70({ guid: s.Guid, channels: [0, 1], stepId: 1 }));
        return (a === b) ? null : (a + ' != ' + b);
    });

    step('STOP', function () {
        var st = new __bte.B.stru_BTS_CMD_STOP();
        st.setChannel([0, 3, 9]);
        var a = __bte.hexOf(st.pack());
        return (a === '0902000000000000') ? null : a;
    });

    step('STEP_JUMP', function () {
        var j = new __bte.B.stru_BTS_CMD_STEP_JUMP();
        j.setChannel([2]); j.JumpStepID = 5;
        var a = __bte.hexOf(j.pack());
        return (a === '040000000000000005') ? null : a;
    });

    step('SET_RTC', function () {
        var r = new __bte.B.stru_BTS_CMD_SET_RTC();
        r.Year = 2026; r.Month = 9; r.Day = 13; r.Hour = 7; r.Minute = 5; r.Second = 30;
        var a = __bte.hexOf(r.pack());
        return (a === '1a090d07051e') ? null : a;
    });

    step('RESUME_CONFIRM', function () {
        var c = new __bte.B.stru_BTS_CMD_RESUME_CONFIRM();
        c.Result = 1;
        var a = __bte.hexOf(c.pack());
        return (a === '01') ? null : a;
    });

    step('DFDATA_RESP76', function () {
        var d = new __bte.B.stru_BTS_CMD_SEND_DFDATA_RESP76();
        d.Index = 88;
        var a = __bte.hexOf(d.pack());
        return (a === '58000000') ? null : a;
    });

    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // -----------------------------------------------------------------------
    // 4. 传输层: NetServer / NetDataManager / NetPack 收发路径
    //
    // 这里**不走真实 socket**: 引擎当前的 socket 栈在第一次 listen/connect 时就是
    // 0xC0000005(见文件头"已知阻塞"), 而且这是仓库级现象 —— 用 test_jsnet.exe 同样
    // 在它的第 5 项(TCP echo)崩。所以本组用 NetDataManager 的纯内存收发路径验证:
    //   packData(组帧) 必须与 protocol.js 的 encodeFrame 逐字节一致;
    //   recvData(喂字节) -> parsePack -> takePack 必须还原出同一帧;
    //   粘包 / 半包 / 坏校验 的处理与 C++ 语义一致(坏包清缓冲)。
    // -----------------------------------------------------------------------
    runCheck("net: NetPackManager factory has NetPackVariant registered", R"JS(
(function () {
    var p = __bte.NetPackManager.instance().createPack(__bte.NETPACK_VARIANT);
    return !!p && p.type() === __bte.NETPACK_VARIANT && typeof p.pack === 'function';
})()
)JS");

    runCheck("net: NetClient module loads (sync waitPack replaced by waitPackAsync)", R"JS(
(function () {
    // QTcpSocket 的 waitForReadyRead/waitForBytesWritten 在 JS 引擎里没有对应物,
    // 位置留给了 Promise 版本; 老的同步签名保留但返回 null(见 netClient.js 头注)。
    return typeof __bte.NetClient === 'function'
        && typeof __bte.NetClient.prototype.waitPackAsync === 'function'
        && typeof __bte.NetClient.prototype.connectToHost === 'function'
        && typeof __bte.NetClient.prototype.sendPack === 'function';
})()
)JS");

    runCheck("net: NetServer construct / idle state (no socket involved)", R"JS(
(function () {
    globalThis.__srv = new __bte.NetServer(null);
    globalThis.__conns = [];
    globalThis.__recv = [];
    __srv.newConnect.push(function (netId) { __conns.push(netId); });
    __srv.newDataRecv.push(function (netId, ptr) {
        __recv.push({ netId: netId, cmd: ptr.info.CmdID, sub: ptr.info.SubDevID, data: ptr.data });
    });
    __srv.stopServer();                 // 没启动过 -> 必须是安全的 no-op
    __srv.closeConnect(1);              // 不存在的会话 -> 安全
    var info = __srv.netInfo(99);       // 不存在的会话 -> ip 为空
    return __srv.isOpen() === false && __srv.boundPort() === 0
        && info.ip === '' && info.port === 0
        && typeof __srv.sendPack === 'function' && typeof __srv.startServer === 'function';
})()
)JS");

    engine.eval(R"JS(
(function () {
    globalThis.__body = __bte.B.packStart70({
        guid: new Uint8Array([0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]),
        channels: [0, 1], stepId: 1 });
    globalThis.__frame = __bte.encodeFrame({
        deviceId: 7, subDevId: 1, cmdId: __bte.CMD.START70, body: __body });
})()
)JS");

    checkEqJs("net: NetDataManager.packData == protocol.js encodeFrame (byte for byte)", R"JS(
(function () {
    var mgr = new __bte.NetDataManager();
    var v = new __bte.NetPackVariant();
    v.info.DeviceID = 7;
    v.info.SubDevID = 1;
    v.info.CmdID = __bte.CMD.START70;
    v.info.Length = 8 + 6 + __body.length + 2;
    v.data = __body;
    return __bte.hexOf(mgr.packData(v));
})()
)JS", "__bte.hexOf(__frame)");

    runCheck("net: recvData -> parsePack -> takePack restores the frame", R"JS(
(function () {
    var mgr = new __bte.NetDataManager();
    mgr.newSession(1, null);
    mgr.recvData(1, __frame);
    var p = mgr.takePack(1);
    return !!p && p.info.CmdID === __bte.CMD.START70 && p.info.SubDevID === 1
        && p.info.Length === __frame.length
        && __bte.hexOf(p.data) === __bte.hexOf(__body)
        && mgr.takePack(1) === null;
})()
)JS");

    runCheck("net: sticky packets (two frames in one read)", R"JS(
(function () {
    var mgr = new __bte.NetDataManager();
    mgr.newSession(1, null);
    var two = new Uint8Array(__frame.length * 2);
    two.set(__frame, 0);
    two.set(__frame, __frame.length);
    mgr.recvData(1, two);
    var a = mgr.takePack(1);
    var b = mgr.takePack(1);
    return !!a && !!b && __bte.hexOf(a.data) === __bte.hexOf(__body)
        && __bte.hexOf(b.data) === __bte.hexOf(__body) && mgr.takePack(1) === null;
})()
)JS");

    runCheck("net: half packet (frame split across two reads)", R"JS(
(function () {
    var mgr = new __bte.NetDataManager();
    mgr.newSession(1, null);
    mgr.recvData(1, __frame.slice(0, 20));
    var none = mgr.takePack(1);          // 半包: 还不该产出
    mgr.recvData(1, __frame.slice(20));
    var p = mgr.takePack(1);
    return none === null && !!p && __bte.hexOf(p.data) === __bte.hexOf(__body);
})()
)JS");

    runCheck("net: bad checksum is dropped (C++ 语义: 清空该段缓冲)", R"JS(
(function () {
    var mgr = new __bte.NetDataManager();
    mgr.newSession(1, null);
    var bad = new Uint8Array(__frame.slice(0));
    bad[bad.length - 2] = 0x00;          // 打坏校验和
    mgr.recvData(1, bad);
    return mgr.takePack(1) === null;
})()
)JS");

    // -----------------------------------------------------------------------
    // 5. 诊断(不计入断言): bteserver 目录下**每个 .js 模块**能否被加载?
    //    依赖的 Qt 类由 qtcompat 空壳兜住, 所以"能否加载"能精确指出移植还差在哪一层:
    //      全部可加载 -> 剩下的是"空壳 API 的真实现"(界面渲染/数据落库)
    //      有失败     -> 还有 import 层就断掉的依赖(如 0x0200 工步包 / sqlite)
    //
    //    这里**只加载、不构造**: BTEServer 的构造函数里会 m_server.startServer(8001)
    //    -> 真实 socket listen, 而引擎当前的 socket 栈会 0xC0000005(见文件头), 一构造
    //    就把测试进程带走。等 socket 栈修好后再在这里加"构造 + 走一遍定时器"。
    // -----------------------------------------------------------------------
    {
        JZDir dir(JZ_BTESERVER_DIR);
        std::vector<std::string> files = dir.entryList({ "*.js" }, JZDir::Files);
        int ok_n = 0, bad_n = 0;
        for (size_t i = 0; i < files.size(); i++)
        {
            const std::string& name = files[i];
            if (name == "main.js")
                continue;                        // 已在引导脚本里加载过
            if (name.size() > 0 && name[0] == '_')
                continue;
            if (engine.runModuleFile(dir.absoluteFilePath(name)))
                ok_n++;
            else
            {
                bad_n++;
                jzDebug() << "[diag] module load FAILED:" << name;
            }
        }
        jzDebug() << "[diag] bteserver module load:" << ok_n << "OK /" << bad_n << "FAILED";
    }

    // BTEServer 的定时器改造(2026-09-13): QTimer + sigTimeout -> JS 原生 setInterval。
    // 这里只检查"改造后的接口在"(不构造 BTEServer: 其 ctor 会 startServer(8001) -> 触发
    // 引擎 socket 栈的 0xC0000005, 见文件头)。
    {
        const string tmp = string(JZ_JS_WRAPPER_DIR) + "/_test_bte_ui.mjs";
        string mjs = "import { BTEServer } from '" + string(JZ_BTESERVER_DIR) + "/BTEServer.js';\n"
            "globalThis.__BTEServer = BTEServer;\n";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f)
        {
            fwrite(mjs.data(), 1, mjs.size(), f);
            fclose(f);
        }
        engine.runModuleFile(tmp);
        remove(tmp.c_str());

        runCheck("BTEServer: timers are JS-native (startTimers/stopTimers, no QTimer)", R"JS(
(function () {
    if (typeof __BTEServer !== 'function') return false;
    var p = __BTEServer.prototype;
    if (typeof p.startTimers !== 'function' || typeof p.stopTimers !== 'function') return false;

    // 真正跑一遍: 用假的槽替换三个槽, startTimers 后应由 setInterval 驱动
    var probe = Object.create(p);
    globalThis.__bt_hb = 0; globalThis.__bt_step = 0;
    probe.onHeartTime = function () { __bt_hb++; };
    probe.onAutoTime = function () {};
    probe.onStepTime = function () { __bt_step++; };
    probe.startTimers();
    var started = (probe.m_heartTimer !== 0) && (probe.m_autoTimer !== 0) && (probe.m_stepTimer !== 0);
    globalThis.__bt_probe = probe;
    return started;
})()
)JS");
        check("BTEServer: startTimers 的 setInterval 真被引擎触发",
              waitFor("__bt_hb > 0 && __bt_step > 0", 2000));
        runCheck("BTEServer: stopTimers 清干净(句柄归 0)", R"JS(
(function () {
    __bt_probe.stopTimers();
    return __bt_probe.m_heartTimer === 0 && __bt_probe.m_autoTimer === 0
        && __bt_probe.m_stepTimer === 0;
})()
)JS");
    }

    // -----------------------------------------------------------------------
    // 6. 缺口账单(不参与断言, 只打印便于人工核对移植进度)
    // -----------------------------------------------------------------------
    engine.eval("__bte.qtCompat.report();");         // Qt 侧: 被调到但还只是空壳的 API
    engine.eval("__bte.blockingIoReport();");        // 传输侧: 被调用到的"同步阻塞 IO"缺口

    netModule.deinitEngine();
    coreModule.deinitEngine();

    if (g_fail == 0)
        jzDebug() << "[test_bteserver] ALL PASS";
    else
        jzDebug() << "[test_bteserver] FAILED:" << g_fail;

    return g_fail == 0 ? 0 : 1;
}
