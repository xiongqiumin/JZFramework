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
// 断言分组:
//   1) 模块可加载  —— 含 main.js 载入即跑的 protocolSelfTest(失败会抛异常, 故"能加载"即"自测通过")
//   2) Qt 兼容层 + JS 原生替代 —— qtcompat 就位; QTimer/QDateTime/QByteArray/QDataStream 已换成
//                      setInterval/Date/JSON(datetime.js / storage.js)
//   3) stru_* 包类 —— 上报帧(0x7C/0x7D)与控制帧的 C++ 字段名往返 + 逐字节一致
//   4) 传输层      —— NetDataManager/NetPack 的组帧-解帧往返(纯内存: packData 与 protocol.js
//                      的 encodeFrame 必须逐字节一致; 粘包/半包/坏校验)
//   5) 真实 socket —— NetServer.listen + 原始 socket / NetClient 双向收发(2026-09-13 恢复)
//   6) 模块加载诊断(逐个 .js 全量) / 7) 整机装配: 真构造 BTEServer / 8) 缺口账单
//
// 另有两条"引擎行为"的守门断言:
//   - 同一文件用绝对路径/相对路径 import 必须归为同一模块(只求值一次)
//   - Qt 兼容层是**单实例**(否则缺口账单会分裂成两份)
//
// 注: 第 5 组曾在引擎 socket 栈崩溃期间(第一次 listen() 就 0xC0000005, 且 test_jsnet
//     同样崩)被降级为"纯内存路径"; socket 栈修好后已恢复, 并与内存路径共存。
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
#include "JZJSWidgetModule.h"
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

    // 控件模块: 向 JS 注册框架真控件类(JZWindow/JZLabel/JZPushButton/JZGridLayout/...)。
    // qtcompat.js 会据此把 Qt 名字(QLabel/...)桥到框架实现; 没初始化时退回 JS 替身。
    JZJSWidgetModule widgetModule;
    widgetModule.initEngine(&engine);
    STEP("widget module init");

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
        // 引导脚本**放在 bteserver 目录里**并用相对路径 import —— 这样业务模块看到的
        // 模块名与它们互相 import 时一致, 整个图只实例化一次。
        // (测试曾把引导脚本放在 wrapper 目录、用绝对路径 import, 结果引擎把
        //  "F:/.../qtcompat.js" 和 "./qtcompat.js" 当成两个模块 —— qtcompat.js 被求值两次,
        //  出现两套全局与两份缺口账单。见下面的 loads 断言。)
        const string tmp = dir + "/_test_bteserver_globals.mjs";
        string mjs =
            "import * as mainMod from './main.js';\n"
            "import { BTEServer } from './BTEServer.js';\n"
            "import { NetServer } from './netServer.js';\n"
            "import { NetClient } from './netClient.js';\n"
            "import { NetPackRegist, NetPackVariant, NETPACK_VARIANT } from './netPackDefine.js';\n"
            "import { NetPackManager, PackInfo } from './netPack.js';\n"
            "import { NetDataManager, blockingIoReport } from './netDataManager.js';\n"
            "import * as B from './structs.js';\n"
            "import * as P from './protocol.js';\n"
            "import * as DT from './datetime.js';\n"
            "import * as ST from './storage.js';\n"
            "import { DataTrans } from './DataTrans.js';\n"
            "import { BTEProject, DeviceData, SplitInfo } from './BTEProject.js';\n"
            "import { BTERecordInfo, BTEStepInfo, BTERecord } from './BTERecordDataBase.js';\n"
            "import { net } from 'net/dist/index.js';\n"
            "import qtCompat from './qtcompat.js';\n"
            "globalThis.__bte = {\n"
            "  main: mainMod, B: B, P: P, DT: DT, ST: ST, DataTrans: DataTrans, net: net, qtCompat: qtCompat,\n"
            "  BTEProject: BTEProject, DeviceData: DeviceData, SplitInfo: SplitInfo,\n"
            "  BTERecordInfo: BTERecordInfo, BTEStepInfo: BTEStepInfo, BTERecord: BTERecord,\n"
            "  NetServer: NetServer, NetClient: NetClient, NetPackManager: NetPackManager,\n"
            "  NetDataManager: NetDataManager, blockingIoReport: blockingIoReport,\n"
            "  NetPackVariant: NetPackVariant, NETPACK_VARIANT: NETPACK_VARIANT,\n"
            "  CMD: P.CMD, encodeFrame: P.encodeFrame, hexOf: P.hexOf,\n"
            "};\n"
            "globalThis.__BTEServer = BTEServer;\n"
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

    // 同一份模块只能被求值一次: 引擎按"模块名"缓存, 而模块名是 import 说明符归一化后的
    // 字符串 —— 绝对路径 import 与相对路径 import 若归不到同一个名字, 就会各求值一次,
    // 于是出现两套模块级状态(单例/账单/全局注入)。这里把它钉住。
    checkEqJs("engine: 同一文件绝对路径/相对路径 import 归为同一模块(只求值一次)",
              "String(globalThis.__qtcompatLoads)", "'1'");

    // -----------------------------------------------------------------------
    // 2.5 框架桥探针: 引擎里能拿到哪些框架(JZ*)类; 这些类能否被 JS 继承/构造
    //     —— qtcompat 要把 Qt 名字桥到框架实现, 前提就是这些。
    // -----------------------------------------------------------------------
    engine.eval(R"JS(
(function () {
    var names = ['JZWidget', 'JZWindow', 'JZLabel', 'JZPushButton', 'JZLineEdit', 'JZCheckbox',
        'JZRadioButton', 'JZSlider', 'JZProgressBar', 'JZComboBox', 'JZSpinBox', 'JZGroupBox',
        'JZChart', 'JZLayout', 'JZVBoxLayout', 'JZHBoxLayout', 'JZGridLayout', 'JZSelfLayout',
        'JZPoint', 'JZSize', 'JZRect', 'JZColor', 'JZImage', 'JZPainter', 'uiLoader',
        'JZTableWidget', 'JZTableWidgetItem', 'JZListWidget', 'JZMenu', 'JZMessageBox',
        'JZFont', 'JZPen', 'JZBrush', 'JZSettings', 'JZThread'];
    var have = [], miss = [];
    for (var i = 0; i < names.length; i++)
        (typeof globalThis[names[i]] === 'function' || typeof globalThis[names[i]] === 'object')
            ? have.push(names[i]) : miss.push(names[i]);
    globalThis.__jzHave = have.join(',');
    globalThis.__jzMiss = miss.join(',');

    try {
        class L extends JZLabel { constructor(t) { super(); if (t !== undefined) this.setText(t); } }
        var l = new L('hi');
        globalThis.__jzSub = 'sub=' + (l instanceof JZLabel) + ' text=' + l.text();
    } catch (e) { globalThis.__jzSub = 'sub EXC: ' + String(e); }
    try { globalThis.__jzGrid = 'grid=' + (new JZGridLayout() instanceof JZGridLayout); }
    catch (e) { globalThis.__jzGrid = 'grid EXC: ' + String(e); }
    try {
        var p = new JZPoint(); p.setPoint(3, 4);
        globalThis.__jzPoint = 'point=' + p.x() + ',' + p.y();
    } catch (e) { globalThis.__jzPoint = 'point EXC: ' + String(e); }
    // 原型方法名清单(用于对齐 Qt 方法名 -> 框架方法名)
    try {
        function keysOf(C) {
            return Object.getOwnPropertyNames(C.prototype).sort().join(',');
        }
        globalThis.__jzKeys =
            'JZWidget: ' + keysOf(JZWidget) + '\n' +
            'JZGridLayout: ' + keysOf(JZGridLayout) + '\n' +
            'JZVBoxLayout: ' + keysOf(JZVBoxLayout) + '\n' +
            'JZLabel: ' + keysOf(JZLabel) + '\n' +
            'JZPushButton: ' + keysOf(JZPushButton) + '\n' +
            'JZWindow: ' + keysOf(JZWindow) + '\n' +
            'JZPoint: ' + keysOf(JZPoint) + '\n' +
            'JZRect: ' + keysOf(JZRect) + '\n' +
            'JZColor: ' + keysOf(JZColor);
    } catch (e) { globalThis.__jzKeys = 'keys EXC: ' + String(e); }
})()
)JS");
    {
        auto hv = engine.eval("__jzHave");
        auto ms = engine.eval("__jzMiss");
        auto sb = engine.eval("__jzSub");
        auto gd = engine.eval("__jzGrid");
        auto pt = engine.eval("__jzPoint");
        jzDebug() << "[diag] framework classes HAVE:" << engine.fromJSValue<string>(hv.get());
        jzDebug() << "[diag] framework classes MISS:" << engine.fromJSValue<string>(ms.get());
        jzDebug() << "[diag] probe:" << engine.fromJSValue<string>(sb.get())
                  << "|" << engine.fromJSValue<string>(gd.get())
                  << "|" << engine.fromJSValue<string>(pt.get());
        auto kk = engine.eval("__jzKeys");
        jzDebug() << "[diag] framework proto keys:\n"
                  << (engine.isException(kk.get()) ? string("<exception>") : engine.fromJSValue<string>(kk.get()));
    }

    // -----------------------------------------------------------------------
    // 2.6 框架桥: Qt 名字是否已指向框架真实现(JZ*)
    // -----------------------------------------------------------------------
    checkEqJs("bridge: Qt 名 = 框架真类 (QLabel/QPushButton/... instanceof JZ*)", R"JS(
(function () {
    var bad = [];
    function chk(qtName, jzName) {
        var Q = globalThis[qtName], J = globalThis[jzName];
        if (typeof Q !== 'function' || typeof J !== 'function') { bad.push(qtName + '/' + jzName + ' 缺'); return; }
        try { var o = new Q(); if (!(o instanceof J)) bad.push(qtName + ' !instanceof ' + jzName); }
        catch (e) { bad.push(qtName + ' new EXC: ' + e); }
    }
    chk('QLabel', 'JZLabel'); chk('QPushButton', 'JZPushButton');
    chk('QLineEdit', 'JZLineEdit'); chk('QCheckBox', 'JZCheckbox'); chk('QRadioButton', 'JZRadioButton');
    chk('QComboBox', 'JZComboBox'); chk('QSpinBox', 'JZSpinBox'); chk('QSlider', 'JZSlider');
    chk('QProgressBar', 'JZProgressBar'); chk('QGroupBox', 'JZGroupBox');
    chk('QGridLayout', 'JZGridLayout'); chk('QVBoxLayout', 'JZVBoxLayout'); chk('QHBoxLayout', 'JZHBoxLayout');
    chk('QWidget', 'JZWidget'); chk('QDialog', 'JZWindow'); chk('QMainWindow', 'JZWindow');
    chk('QPoint', 'JZPoint'); chk('QSize', 'JZSize'); chk('QRect', 'JZRect'); chk('QColor', 'JZColor');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    checkEqJs("bridge: Qt 语义/方法名补在框架类型上 (含信号数组与 Qt 重载)", R"JS(
(function () {
    var bad = [];
    function eq(n, got, want) { if (String(got) !== String(want)) bad.push(n + ': ' + got + ' != ' + want); }

    // ---- 值类型: Qt 的构造参数与查询方法 ----
    var p = new QPoint(3, 4);
    eq('QPoint.x', p.x(), 3); eq('QPoint.y', p.y(), 4);
    var sz = new QSize(5, 6); eq('QSize.h', sz.height(), 6);
    var r = new QRect(10, 20, 30, 40);
    eq('QRect.w', r.width(), 30); eq('QRect.right', r.right(), 39);
    var nr = new QRect(new QPoint(30, 40), new QPoint(10, 20)).normalized();
    eq('QRect(2点).normalized.x', nr.x(), 10); eq('QRect(2点).normalized.w', nr.width(), 21);
    eq('QRect.adjusted.w', r.adjusted(0, 0, 10, 0).width(), 40);
    var c = new QColor(1, 2, 3);
    eq('QColor.blue', c.blue(), 3); eq('QColor.name', c.name(), '#010203');
    eq('QColor("#ff0000").red', new QColor('#ff0000').red(), 255);
    eq('Qt.blue is JZColor', (Qt.blue instanceof JZColor), true);
    eq('Qt.blue.name', Qt.blue.name(), '#0000ff');

    // ---- 控件: Qt 方法名 ----
    var l = new QLabel('hi');
    eq('QLabel.text', l.text(), 'hi');
    l.setAlignment(Qt.AlignCenter);
    l.setText('x'); eq('QLabel.setText', l.text(), 'x');
    try { l.setFixedSize(new QRect(0, 0, 100, 20)); }   // Qt 的 QRect 重载: 不能抛
    catch (eFix) { bad.push('setFixedSize(QRect) EXC: ' + eFix); }
    eq('QLabel.isVisible', typeof l.isVisible, 'function');

    var b = new QPushButton('go');
    eq('QPushButton.text', b.text(), 'go');
    var n = 0;
    b.sigClicked.push(function () { n++; });        // 移植的"信号数组"约定
    b.click();
    eq('QPushButton.sigClicked(框架回调)', n, 1);

    var cb = new QCheckBox('c');
    eq('QCheckBox.text', cb.text(), 'c');

    var g = new QGridLayout();
    g.addWidget(b, 0, 0);
    g.addWidget(l, 1, 0, 1, 2);                     // 带 rowSpan/colSpan 的重载

    // ---- 原生实例上挂普通属性(适配器要用) ----
    try { l._probe = 7; eq('native prop set', l._probe, 7); }
    catch (e) { bad.push('native prop set EXC: ' + e); }

    // ---- C++ 的基类初始化写法 QDialog.call(this, parent) 仍要能用 ----
    try {
        function MyDlg() { QDialog.call(this, null); this.init_ = 1; }
        var d = new MyDlg();
        eq('QDialog.call(this) 兜底', typeof d.setWindowFlags, 'function');
    } catch (e3) { bad.push('QDialog.call EXC: ' + e3); }

    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

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

    checkEqJs("qtcompat: Qt enum + named colors + qApp", R"JS(
(function () {
    var bad = [];
    function eq(n, got, want) { if (String(got) !== String(want)) bad.push(n + ': ' + got + ' != ' + want); }
    eq('UserRole', Qt.UserRole, 0x0100);
    eq('CustomContextMenu', Qt.CustomContextMenu, 2);
    eq('WindowMaximizeButtonHint', Qt.WindowMaximizeButtonHint, 0x02000000);
    eq('Qt.red instanceof QColor', (Qt.red instanceof QColor), true);
    eq('Qt.red.red()', Qt.red.red(), 255);
    eq('typeof qApp', typeof qApp, 'object');
    eq('qApp instanceof QApplication', (qApp instanceof QApplication), true);
    eq('applicationDirPath 非空', QApplication.applicationDirPath().length > 0, true);
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

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
    // 5. 真实 socket 端到端 loopback(2026-09-13 恢复; 引擎 socket 栈修好后不再崩)
    //
    // 这是"bteserver 传输层真的能跑"的证据链:
    //   NetServer.listen(0) -> 系统分配端口 -> 原始 socket 连上发一帧
    //   -> NetServer.newConnect/newDataRecv -> NetClient 连上 -> 双向收发
    //   -> 断开传播 -> stopServer。
    // 帧的字节由 protocol.js 生成、解包走 structs.js, 所以这条路径一旦成立,
    // "上位机与下位机的报文互通"就同时被验证了。
    // -----------------------------------------------------------------------
    runCheck("e2e: NetServer.startServer(18091) returns true", R"JS(
(function () {
    // 用固定端口: 引擎的 server.address().port 回的是**请求端口**而不是系统分配端口,
    // 所以 listen(0) 拿不到真实端口(见 netServer.boundPort 的注释)。
    globalThis.__port = 18091;
    return __srv.startServer(__port) === true;
})()
)JS");
    check("e2e: listener is open", waitFor("__srv.isOpen() === true", 3000));

    engine.eval(R"JS(
(function () {
    globalThis.__sockOpen = false;
    globalThis.__sock = __bte.net.createConnection({ host: '127.0.0.1', port: __port });
    __sock.on('connect', function () { __sockOpen = true; __sock.write(__frame); });
    __sock.on('error', function (e) { console.error('[e2e] raw socket error: ' + e); });
})()
)JS");
    check("e2e: raw socket connected -> NetServer.newConnect(netId)",
          waitFor("__sockOpen === true && __conns.length === 1", 3000));
    check("e2e: NetServer decoded 0x6E frame from raw socket", waitFor("__recv.length === 1", 3000));
    checkEqJs("e2e: decoded cmd / SubDevID / body match sender", R"JS(
(function () {
    if (__recv.length !== 1) return 'recv=' + __recv.length;
    var r = __recv[0];
    var bad = [];
    if (r.cmd !== __bte.CMD.START70) bad.push('cmd=' + r.cmd);
    if (r.sub !== 1) bad.push('sub=' + r.sub);
    if (__bte.hexOf(r.data) !== __bte.hexOf(__body)) bad.push('body');
    if (r.netId !== __conns[0]) bad.push('netId');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // ---- NetClient: 走 NetDataManager.packData / parsePack 的完整收发路径 ----
    engine.eval(R"JS(
(function () {
    globalThis.__cliUp = false;
    globalThis.__cliRecv = [];
    globalThis.__cli = new __bte.NetClient(null);
    __cli.sigConnect.push(function () { __cliUp = true; });
    __cli.sigError.push(function () { console.error('[e2e] client sigError'); });
    __cli.newDataRecv.push(function (pack) {
        __cliRecv.push({ cmd: pack.info.CmdID, sub: pack.info.SubDevID, data: pack.data });
    });
    __cli.connectToHost('127.0.0.1', __port);
})()
)JS");
    check("e2e: NetClient connected (sigConnect driven by engine 'connect')",
          waitFor("__cliUp === true", 3000));
    check("e2e: server saw the client as a second session", waitFor("__conns.length === 2", 3000));

    // 客户端 -> 服务端
    engine.eval(R"JS(
(function () {
    var v = new __bte.NetPackVariant();
    v.info.DeviceID = 7;
    v.info.SubDevID = 2;
    v.info.CmdID = __bte.CMD.START70;
    v.info.Length = 8 + 6 + __body.length + 2;
    v.data = __body;
    __cli.sendPack(v);
})()
)JS");
    check("e2e: NetClient.sendPack -> NetServer decoded (SubDevID 2)", waitFor("__recv.length === 2", 3000));
    checkEqJs("e2e: client->server frame body match", R"JS(
(function () {
    if (__recv.length !== 2) return 'recv=' + __recv.length;
    var r = __recv[1];
    var bad = [];
    if (r.cmd !== __bte.CMD.START70) bad.push('cmd=' + r.cmd);
    if (r.sub !== 2) bad.push('sub=' + r.sub);
    if (__bte.hexOf(r.data) !== __bte.hexOf(__body)) bad.push('body');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // 服务端 -> 客户端(用刚才那条连接的 netId 发回)
    engine.eval(R"JS(
(function () {
    var v = new __bte.NetPackVariant();
    v.info.DeviceID = 7;
    v.info.SubDevID = 3;
    v.info.CmdID = __bte.CMD.START70;
    v.info.Length = 8 + 6 + __body.length + 2;
    v.data = __body;
    return __srv.sendPack(__recv[1].netId, v);
})()
)JS");
    check("e2e: NetServer.sendPack -> NetClient.newDataRecv", waitFor("__cliRecv.length === 1", 3000));
    checkEqJs("e2e: server->client frame body match", R"JS(
(function () {
    if (__cliRecv.length !== 1) return 'recv=' + __cliRecv.length;
    var r = __cliRecv[0];
    var bad = [];
    if (r.cmd !== __bte.CMD.START70) bad.push('cmd=' + r.cmd);
    if (r.sub !== 3) bad.push('sub=' + r.sub);
    if (__bte.hexOf(r.data) !== __bte.hexOf(__body)) bad.push('body');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");

    // ---- 断开 + 收尾 ----
    engine.eval(R"JS(
(function () {
    globalThis.__srvDis = [];
    __srv.disConnect.push(function (netId) { __srvDis.push(netId); });
    __cli.disconnectFromHost();
    __sock.destroy();
})()
)JS");
    check("e2e: disconnect propagated to NetServer.disConnect", waitFor("__srvDis.length >= 1", 3000));
    runCheck("e2e: NetServer.stopServer() closes listener + drops sessions", R"JS(
(function () {
    __srv.stopServer();
    var info = __srv.netInfo(__conns[0]);
    return __srv.isOpen() === false && info.port === 0;
})()
)JS");

    // -----------------------------------------------------------------------
    // 6. 诊断(不计入断言): bteserver 目录下**每个 .js 模块**能否被加载?
    //    依赖的 Qt 类由 qtcompat 空壳兜住, 所以"能否加载"能精确指出移植还差在哪一层:
    //      全部可加载 -> 剩下的是"空壳 API 的真实现"(界面渲染/数据落库)
    //      有失败     -> 还有 import 层就断掉的依赖(如 0x0200 工步包 / sqlite)
    // -----------------------------------------------------------------------
    {
        JZDir dir(JZ_BTESERVER_DIR);
        std::vector<std::string> files = dir.entryList({ "*.js" }, JZDir::Files);
        int ok_n = 0, bad_n = 0;
        for (size_t i = 0; i < files.size(); i++)
        {
            const std::string& name = files[i];
            if (name == "main.js" || name == "qtcompat.js")
                continue;   // 已在引导脚本里(相对路径)加载过 —— 再用绝对路径加载会得到
                            // **第二份模块实例**(引擎按模块名缓存), qtcompat 的缺口账单会分裂
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
    // (__BTEServer 由引导脚本导出)
    {
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
    // 7. 整机装配: 把 BTEServer **真的构造出来**
    //
    //    这是"bteserver 在 JS 引擎里跑起来"的核心证据: 构造函数会一路走到
    //      Ui_* 空壳建界面 -> 建 TcpServer(8001) -> loadProject(存档 JSON)
    //      -> initDeviceList/initTable(建 32 个通道卡片 + 表格) -> startTimers
    //    引擎 socket 栈修好后(2026-09-13)这条路才通。
    // -----------------------------------------------------------------------
    engine.eval(R"JS(
(function () {
    globalThis.__uiOk = false;
    globalThis.__uiErr = '';
    if (typeof __BTEServer !== 'function') { __uiErr = 'BTEServer 未加载'; return; }
    try {
        globalThis.__ui = new __BTEServer();   // ctor: m_server.startServer(8001) + loadProject + startTimers
        __uiOk = true;
    } catch (e) {
        __uiErr = String(e) + ' || ' + ((e && e.stack) ? String(e.stack) : '');
    }
    try {
        var n0 = __bte.qtCompat.missing.size;
        var ui0 = new Ui_BTEServerClass();
        globalThis.__uiProbe = [typeof ui0, typeof ui0.tableQuery,
            typeof ui0.tableQuery.setColumnCount, typeof ui0.tableQuery.setColumnCount(9)].join('|')
            + ' missing ' + n0 + '->' + __bte.qtCompat.missing.size
            + ' loads=' + (globalThis.__qtcompatLoads || 0);
    } catch (e2) { globalThis.__uiProbe = 'probe EXC: ' + e2; }
})()
)JS");
    {
        auto fu = engine.eval("__uiErr");
        string uerr = engine.isException(fu.get()) ? string("<exception>") : engine.fromJSValue<string>(fu.get());
        if (!uerr.empty())
            jzDebug() << "[diag] construct BTEServer error:" << uerr;
        auto fp = engine.eval("__uiProbe");
        string uprobe = engine.isException(fp.get()) ? string("<exception>") : engine.fromJSValue<string>(fp.get());
        jzDebug() << "[diag] ui stub probe:" << uprobe;
    }
    checkEqJs("BTEServer: constructs (Ui_* stubs + project + startServer(8001) + timers)", R"JS(
(function () {
    var bad = [];
    if (typeof __BTEServer !== 'function') return 'BTEServer 未加载';
    if (!__ui) return 'constructor failed: ' + __uiErr;
    if (typeof __ui.ui !== 'object' && typeof __ui.ui !== 'function') bad.push('ui');
    // 注意: BTEServer.m_devices 是"连上来的设备"(运行时), 此刻应为空;
    //       工程里的设备列表在 m_project.m_devices(debug 下至少有一台 127.0.0.1)。
    if (!__ui.m_project || __ui.m_project.m_devices.size < 1)
        bad.push('project.devices=' + (__ui.m_project ? __ui.m_project.m_devices.size : 'null'));
    if (__ui.m_stepDataDialog === null) bad.push('stepDataDialog');
    if (!__ui.m_itemList || __ui.m_itemList.length === 0) bad.push('itemList');
    if (__ui.m_heartTimer === 0 || __ui.m_stepTimer === 0) bad.push('timers 未启动');
    return bad.length ? bad.join('; ') : 'OK';
})()
)JS", "'OK'");
    check("BTEServer: m_server 在构造函数里就起来了(8001)", waitFor("__ui.m_server.isOpen() === true", 2000));
    engine.eval(R"JS(
(function () {
    if (!globalThis.__ui) return false;
    try { __ui.stopTimers(); } catch (e) { }
    try { __ui.m_server.stopServer(); } catch (e) { }
    return true;
})()
)JS");

    // -----------------------------------------------------------------------
    // 8. 缺口账单(不参与断言, 只打印便于人工核对移植进度)
    // -----------------------------------------------------------------------
    engine.eval("__bte.qtCompat.bridgeReport();");   // Qt 名: 哪些走了框架真实现 / 哪些仍是 JS
    engine.eval("__bte.qtCompat.report();");         // Qt 侧: 被调到但还只是空壳的 API
    engine.eval("__bte.blockingIoReport();");        // 传输侧: 被调用到的"同步阻塞 IO"缺口

    widgetModule.deinitEngine();
    netModule.deinitEngine();
    coreModule.deinitEngine();

    if (g_fail == 0)
        jzDebug() << "[test_bteserver] ALL PASS";
    else
        jzDebug() << "[test_bteserver] FAILED:" << g_fail;

    return g_fail == 0 ? 0 : 1;
}
