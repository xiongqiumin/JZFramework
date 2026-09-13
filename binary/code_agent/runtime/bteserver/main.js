// ---------------------------------------------------------------------------
// main.js —— 对应 C++ BTEServer/main.cpp, 并合并了原先根目录下独立的协议离线自测
//            (原独立自测入口已删除, 断言并入本文件的 protocolSelfTest(), 载入本文件即运行)
//
// 一份入口两条职责:
//   职责1 protocolSelfTest(): 校验 protocol.js(帧编解码/分帧) 与 structs.js(各命令 pack/unpack)
//        是否与 JZBTEServer 的 C++ 实现逐字节一致。全部标准 API(Uint8Array + DataView),
//        不需要 socket/中位机参与; 末尾打印稳定契约 `BTE_SELFTEST pass=.. fail=..`。
//   职责2 main(): 崩溃处理(CoreDump) + Qt 日志接管 + 语言切换 + 包类型注册 + 单实例 + 主窗启动。
//
// 为什么界面依赖用动态 import(): 翻译层里的 BTEServer.js / netPackDefine.js 还 import 着
//   尚未补齐的 ui_*(见 MEMORY 的"源级镜像层"说明), **静态** import 会在链接期直接
//   失败, 连上面的协议自测都跑不到; 动态 import() 把失败推迟到真正启动界面时。
//
// 2026-09-13 变更(为了"能在 JS 引擎里真的跑起来"):
//   1) 顶部 `import './qtcompat.js'`: 本文件用到的 QApplication/QTranslator/QSettings/
//      QMessageBox/qApp/Qt 由 Qt 兼容层提供(引擎里没有 Qt)。
//   2) CoreDump.js / SecurityDLL.js / securityclient.js **这三个文件在仓库里根本不存在**
//      (都按用户要求未移植), 原来的 `await import(...)` 会直接抛"模块找不到", 使 main()
//      连第一行都跑不到。改为 optionalImport(): 找不到就跳过并记账。
//   3) 新增 startHeadlessServer(port): **不依赖界面**的一条真实可跑入口
//      (NetServer + 协议解析 + 数据包注册), 用于在无 GUI 的引擎会话里跑 BTE 服务端。
//
// Qt -> JS: QApplication -> qtcompat; QTranslator / QSharedMemory -> qtcompat;
//   Qt 消息处理器(type/context/msg) -> 同名函数, 参数语义不变; `#if 0` 的授权校验
//   (SecurityDLL/CheckRegTime/SecurityClient)按原样保留在注释块里。
//   原 LogManager.js 已删除: LogInfo/LogError/LogFatal -> console.log/error;
//   LogManager.instance().init/deinit/setLogLevel -> 由引擎 console 接管(不再单独配目录/级别)。
// ---------------------------------------------------------------------------

import './qtcompat.js';                     // QApplication / qApp / QTranslator / QSettings / Qt
import { CMD, FrameParser, encodeFrame, decodeFrame, hexOf, cmdName, channelMask, maskChannels } from './protocol.js';
import {
    packStart70, packStop, packResume, packStepJump, packDFDataResp, packSetRtc,
    unpackBody, unpackRTData76, unpackDFData76, unpackSendInfo, packRTData76, packDFData76,
    workStatusText, isChannelFault, aTimeToISO, SIZE,
} from './structs.js';

// ===========================================================================
// 职责 1: 协议离线自测(合并自原根目录 main.js)
//
// 帧格式的风险集中在"同步头/长度/校验和/尾/半包"这几处, 只要有硬编码的期望字节做对比,
// 就能在没有真实中位机的情况下确认实现正确。
// ===========================================================================
export function protocolSelfTest()
{
    // ---- 迷你断言 ----
    let g_pass = 0, g_fail = 0;
    function check(cond, name, extra) {
        if (cond) {
            g_pass++;
            console.log('  PASS  ' + name);
        } else {
            g_fail++;
            console.log('  FAIL  ' + name + (extra ? ('\n        ' + extra) : ''));
        }
    }
    function checkEq(actual, expected, name) {
        const ok = actual === expected;
        check(ok, name, ok ? '' : ('期望: ' + expected + '\n        实际: ' + actual));
    }
    function section(title) { console.log('\n==== ' + title + ' ===='); }

    // -----------------------------------------------------------------------
    // 1. 组帧字节级比对 —— 硬编码期望 hex, 锁死同步头/长度/校验和/尾
    // -----------------------------------------------------------------------
    section('1. 组帧(硬编码期望字节)');

    const GUID = new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
    const startBody = packStart70({ guid: GUID, channels: [0, 1], stepId: 1 });
    checkEq(startBody.length, 25, 'packStart70 body = Guid(16)+ChannelID(8)+StepID(1) = 25');

    // 手工算出的期望帧:
    //   sync   8 × 0xFF
    //   head   01 00 | 29 00(Length=41) | 6E 00(CmdID)
    //   body   guid 00..0F | mask 03 00*7 | stepId 01
    //   chksum Σ(head+body) = 0x114 -> 0x14
    //   tail   5A
    const EXPECT_START = 'ffffffffffffffff'
        + '010029006e00'
        + '000102030405060708090a0b0c0d0e0f'
        + '0300000000000000'
        + '01'
        + '14'
        + '5a';
    const startFrame = encodeFrame({ deviceId: 1, subDevId: 0, cmdId: CMD.START70, body: startBody });
    checkEq(hexOf(startFrame), EXPECT_START, 'encodeFrame(START70) 字节与手工推算一致');

    const back = decodeFrame(startFrame);
    checkEq(back.length, 41, 'decodeFrame 读回 Length');
    checkEq(back.cmdId, CMD.START70, 'decodeFrame 读回 CmdID');
    checkEq(back.subDevId, 0, 'decodeFrame 读回 SubDevID');
    checkEq(hexOf(back.body), hexOf(startBody), 'decodeFrame 读回 body');

    // 接收方向: 包头多一个 Result 字节 => Length 也大 1
    const respFrame = encodeFrame({ deviceId: 1, subDevId: 2, cmdId: CMD.START_RESP70, result: 0, body: new Uint8Array(16 + 1 + 5) });
    checkEq(respFrame[8 + 6 + 0] /* Result 位置 */, 0, '接收包包头含 Result 字节');
    checkEq(respFrame.length, 8 + 7 + 22 + 2, '接收包总长 = 8 + 7(包头) + body + 2');

    // -----------------------------------------------------------------------
    // 2. 分帧器: 粘包 / 半包 / 脏数据
    // -----------------------------------------------------------------------
    section('2. 分帧(粘包/半包/脏数据)');

    {   // 半包: 一帧切成 3 段喂入
        const p = new FrameParser();
        const cut1 = startFrame.slice(0, 5);
        const cut2 = startFrame.slice(5, 30);
        const cut3 = startFrame.slice(30);
        checkEq(p.push(cut1).length, 0, '半包: 第 1 段不产出(只有半个同步头)');
        checkEq(p.push(cut2).length, 0, '半包: 第 2 段不产出(包头未完)');
        const got = p.push(cut3);
        checkEq(got.length, 1, '半包: 第 3 段产出 1 帧');
        checkEq(got.length === 1 ? got[0].cmdId : -1, CMD.START70, '半包: 产出的帧命令字正确');
    }

    {   // 粘包: 两帧一次喂入
        const p = new FrameParser();
        const two = new Uint8Array(startFrame.length * 2);
        two.set(startFrame, 0); two.set(startFrame, startFrame.length);
        const got = p.push(two);
        checkEq(got.length, 2, '粘包: 一次喂入 2 帧产出 2 帧');
        checkEq(p.push(new Uint8Array(0)).length, 0, '粘包: 缓冲已清空, 再喂空不产出');
    }

    {   // 脏数据 + 校验和错 + 尾部错, 后面跟一个好帧 => 仍能恢复
        const p = new FrameParser();
        const bad = new Uint8Array(startFrame.slice(0));
        bad[bad.length - 2] = 0x00;                 // 故意打坏校验和
        const dirty = new Uint8Array([0x11, 0x22, 0x33, 0x44, 0x55]);
        const stream = new Uint8Array(dirty.length + bad.length + startFrame.length);
        stream.set(dirty, 0);
        stream.set(bad, dirty.length);
        stream.set(startFrame, dirty.length + bad.length);

        const got = p.push(stream);
        checkEq(got.length, 1, '脏数据+坏校验: 仍能解析出后面那个好帧');
        check(got.length === 1 && got[0].cmdId === CMD.START70, '脏数据: 恢复出的帧命令字正确');
        check(p.stats.badChecksum === 1, '脏数据: 校验和错误被计数', JSON.stringify(p.stats));
        check(p.stats.droppedBytes >= 5 + 1, '脏数据: 丢弃字节被统计', JSON.stringify(p.stats));
    }

    {   // 假的同步头(8×FF 后跟随非法长度) 也要能跳过
        const p = new FrameParser();
        const fake = new Uint8Array(8 + 6);
        fake.fill(0xff, 0, 8);
        fake[8] = 1; fake[9] = 0;
        fake[10] = 0x02; fake[11] = 0x00;   // Length = 2 -> 非法
        fake[12] = 0xff; fake[13] = 0xff;
        const merged = new Uint8Array(fake.length + startFrame.length);
        merged.set(fake, 0); merged.set(startFrame, fake.length);
        const got = p.push(merged);
        checkEq(got.length, 1, '假同步头: 跳过非法长度后仍能解出真帧');
        check(p.stats.badLength >= 1, '假同步头: 非法长度被计数', JSON.stringify(p.stats));
    }

    {   // 假同步头 + 离谱长度(65535): 必须当成假同步头跳过, 不能"等一个永远不来的半包"
        const p = new FrameParser();
        const fake = new Uint8Array(8 + 6);
        fake.fill(0xff, 0, 8);
        fake[8] = 1; fake[9] = 0;
        fake[10] = 0xff; fake[11] = 0xff;   // Length = 65535
        fake[12] = 0x11; fake[13] = 0x00;
        const merged = new Uint8Array(fake.length + startFrame.length);
        merged.set(fake, 0); merged.set(startFrame, fake.length);
        const got = p.push(merged);
        checkEq(got.length, 1, 'Length=65535 的假同步头: 跳过并解出后面的真帧');
        check(p.stats.falseSync >= 1, '假同步头: 超长 Length 被计数', JSON.stringify(p.stats));
    }

    // -----------------------------------------------------------------------
    // 3. 实时帧 0x7C 往返
    // -----------------------------------------------------------------------
    section('3. 实时帧 RTDATA76 往返');

    const rtSrc = {
        dfDataPointer: 1234,
        rtDataCount: 1,
        dataFlag: 0x55,
        channelIdByNum: 3,
        auxChlIdByNum: 0,
        cycleStatus: 2,
        stepId: 5,
        stepType: 1,             // 恒流充电
        workType: 0x10,          // bit4=1 -> 运行
        testTime: 123456789,
        voltage: 40500,          // 0.1mV -> 4.05 V
        current: 250000,
        internalResistance: 1200,
        tempature: 253,          // 25.3 ℃
        chargeCap: 1800000000,
        disChargeCap: 0,
        chargeEng: 7200000000,
        disChargeEng: 0,
        curStepRange: 3000,
        prtCode: 0,
    };
    const rtBody = packRTData76(rtSrc);
    checkEq(rtBody.length, SIZE.RTDATA76, 'packRTData76 = 75 字节');

    const rtFrame = encodeFrame({ deviceId: 7, subDevId: 1, cmdId: CMD.SEND_RTDATA76, body: rtBody });
    const rtParsed = new FrameParser().push(rtFrame);
    checkEq(rtParsed.length, 1, '实时帧被分帧器解出');
    const rt = unpackRTData76(rtParsed[0].body);
    checkEq(rt.dfDataPointer, rtSrc.dfDataPointer, 'RTDATA.dfDataPointer');
    checkEq(rt.channelIdByNum, rtSrc.channelIdByNum, 'RTDATA.channelIdByNum');
    checkEq(rt.testTime.toString(), '123456789', 'RTDATA.testTime(int64)');
    checkEq(rt.chargeCap.toString(), '1800000000', 'RTDATA.chargeCap(int64)');
    checkEq(rt.chargeEng.toString(), '7200000000', 'RTDATA.chargeEng(int64)');
    checkEq(rt.tempature, 253, 'RTDATA.tempature(short)');
    checkEq(rt.prtCode, 0, 'RTDATA.prtCode');
    checkEq(rtParsed[0].subDevId, 1, '帧头 SubDevID 与 body 里的通道序号是两回事(SubDevID=1)');
    checkEq(workStatusText(rt.workType), '运行', 'WorkType bit5..4 = 运行');
    check(!isChannelFault(rt), 'RTDATA 无故障(bit7=0/PrtCode=0)');

    // 故障帧: bit7 置位
    const rtFault = unpackRTData76(packRTData76(Object.assign({}, rtSrc, { workType: 0x90, prtCode: 0x12 })));
    check(isChannelFault(rtFault), 'RTDATA bit7 + PrtCode 被判为通道故障');

    // -----------------------------------------------------------------------
    // 4. 记录点数据 0x7D 往返(2 条)
    // -----------------------------------------------------------------------
    section('4. 记录点数据 DFDATA76 往返');

    const dfSrc = {
        dfDataPointer: 88,
        dataList: [
            {
                dataFlag: 0x59, subDevId: 1, channelIdByNum: 3, auxChlIdByNum: 0,
                testId: 4200, dataSerial: 1, cycleStatus: 0, stepId: 1, stepType: 1,
                stepChangeCount: 0, workType: 0x10, testTime: 1000,
                voltage: 41230, current: 250000, internalResistance: 0, tempature: 250,
                chargeCap: 100, disChargeCap: 0, chargeEng: 0, disChargeEng: 0,
                aTime: { year: 2026, month: 9, day: 13, hour: 7, minute: 5, second: 30 },
                curStepRange: 3000,
            },
            {
                dataFlag: 0x55, subDevId: 1, channelIdByNum: 3, auxChlIdByNum: 0,
                testId: 4200, dataSerial: 7, cycleStatus: 0, stepId: 2, stepType: 2,
                stepChangeCount: 1, workType: 0x10, testTime: 2000,
                voltage: 39800, current: -250000, internalResistance: 1100, tempature: 260,
                chargeCap: 200, disChargeCap: 50, chargeEng: 0, disChargeEng: 0,
                aTime: { year: 2026, month: 9, day: 13, hour: 30, minute: 5, second: 40 },  // btHour=30 => 跨天
                curStepRange: 3000,
            },
        ],
    };
    const dfBody = packDFData76(dfSrc);
    checkEq(dfBody.length, 1 + 4 + 2 * SIZE.DFDATA_DATA, 'packDFData76 = 1 + 4 + 2×83 = 171 字节');

    const dfFrame = encodeFrame({ deviceId: 7, subDevId: 1, cmdId: CMD.SEND_DFDATA76, body: dfBody });
    const dfParsed = new FrameParser().push(dfFrame);
    const df = unpackDFData76(dfParsed[0].body);
    checkEq(df.dfDataNumber, 2, 'DFDATA.dfDataNumber');
    checkEq(df.dataList.length, 2, 'DFDATA.dataList 条数');
    checkEq(df.dataList[0].testId, 4200, 'DFDATA[0].testId(uint)');
    checkEq(df.dataList[0].voltage, 41230, 'DFDATA[0].voltage');
    checkEq(df.dataList[1].current, -250000, 'DFDATA[1].current(负值/有符号)');
    checkEq(df.dataList[0].chargeCap.toString(), '100', 'DFDATA[0].chargeCap(int64)');
    checkEq(aTimeToISO(df.dataList[0].aTime), '2026-09-13 07:05:30', 'ATime_t -> 时间字符串');
    checkEq(aTimeToISO(df.dataList[1].aTime), '2026-09-14 06:05:40', 'ATime_t: hour=30 按天进位');

    // 回应
    checkEq(hexOf(packDFDataResp(88)), '58000000', 'packDFDataResp(88) = Index(4) LE');

    // -----------------------------------------------------------------------
    // 5. 控制类命令的 body 布局
    // -----------------------------------------------------------------------
    section('5. 控制类命令 body');

    checkEq(hexOf(packStop({ channels: [0, 3, 9] })), '0902000000000000', 'packStop(通道 0/3/9) 位图');
    const stopBack = maskChannels(packStop({ channels: [0, 3, 9] }));
    checkEq(stopBack.join(','), '0,3,9', 'maskChannels 还原通道号');
    checkEq(hexOf(packResume({ channels: [1] })), '0200000000000000', 'packResume(通道 1)');
    checkEq(hexOf(packStepJump({ channels: [2], jumpStepId: 5 })), '040000000000000005', 'packStepJump = 位图(8) + 工步号(1)');
    checkEq(hexOf(packSetRtc({ year: 2026, month: 9, day: 13, hour: 7, minute: 5, second: 30 })), '1a090d07051e', 'packSetRtc = 年月日时分秒(年自 2000)');

    // 分派解包: 未知命令不抛异常
    const disp = unpackBody(CMD.SEND_DFDATA76, dfBody);
    checkEq(disp.name, 'DFDATA76', 'unpackBody 按命令名分派');
    checkEq(unpackBody(0x1234, new Uint8Array([1, 2])).name, 'UNKNOWN', 'unpackBody 未知命令返回 UNKNOWN');

    // -----------------------------------------------------------------------
    // 6. 解析结果演示(打印一帧实时数据)
    // -----------------------------------------------------------------------
    section('6. 解析结果演示');
    function showRT(ip, frame) {
        const v = unpackRTData76(frame.body);
        const volt = (v.voltage / 10000).toFixed(4) + ' V';   // DataTrans.h: 电压 0.1mV 为单位
        const temp = (v.tempature / 10).toFixed(1) + ' ℃';    // DataTrans.h: 实际温度 = temp/10
        console.log('  帧头: ip=' + ip + ' SubDevID=' + frame.subDevId + ' Cmd=' + cmdName(frame.cmdId) +
            ' Length=' + frame.length + ' 校验和=OK');
        console.log('  通道: ' + (frame.subDevId + 1) + '-' + v.channelIdByNum +
            '   工步=' + v.stepId + '(类型' + v.stepType + ')' +
            '   循环=' + v.cycleStatus +
            '   状态=' + workStatusText(v.workType) +
            '   故障=' + (isChannelFault(v) ? '是(保护码0x' + v.prtCode.toString(16) + ')' : '否'));
        console.log('  数据: 电压=' + volt + '  温度=' + temp +
            '  时间=' + v.testTime + 'ms  电流量程=' + v.curStepRange + 'mA');
        console.log('  容量: 充=' + v.chargeCap + ' 放=' + v.disChargeCap +
            '  能量: 充=' + v.chargeEng + ' 放=' + v.disChargeEng + '   (电流/容量/能量单位换算待移植 DataTrans)');
    }
    showRT('192.168.1.100', rtParsed[0]);
    console.log('  原始帧 hex: ' + hexOf(rtParsed[0].raw));
    console.log('  报文长度自检: ' + (8 + 6 + rtParsed[0].body.length + 2) + ' = Length ' + rtParsed[0].length);

    // -----------------------------------------------------------------------
    // 汇总
    // -----------------------------------------------------------------------
    console.log('\n==== 汇总: ' + g_pass + ' 通过 / ' + g_fail + ' 失败 ====');
    // 给 C++ 侧测试的稳定契约(纯 ASCII, 不受语言/编码影响):
    // 先打标记再抛异常 —— 即使下面抛了, 标记也在, 测试能区分"跑到末尾了但有用例失败"
    // 与"模块根本没跑起来"。
    console.log('BTE_SELFTEST pass=' + g_pass + ' fail=' + g_fail);
    if (g_fail > 0)
        throw new Error('bteserver 协议解析自测失败 ' + g_fail + ' 项');
}

// ===========================================================================
// 职责 2: 程序入口(对应 C++ BTEServer/main.cpp)
// ===========================================================================

// static BTEServer *g_server = NULL;
let g_server = null;
// QtMessageHandler qtLogHandler = NULL;
let qtLogHandler = null;

// void exception_exit()
export function exception_exit()
{
    // LogManager.instance().deinit();   // 日志改由引擎 console 接管(LogManager.js 已删除)
    g_server.CrashExit();
    QMessageBox.information(null, "", qApp.translate("main", "程序异常，请联系工程师"));
}

// void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
export function myMessageOutput(type, context, msg)
{
    switch (type)
    {
        case QtDebugMsg:
        case QtInfoMsg:
        case QtWarningMsg:
        case QtCriticalMsg:
            qtLogHandler(type, context, msg);
            break;
        case QtFatalMsg:
            console.error("%s", msg);
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
            break;
    }
}

// void parseDataTest();
//   原 main.cpp 声明的自由函数, 实现文件(ParesDataTest.js, 离线抓包解析自测)已按要求删除,
//   故此处的调用点一并去掉 —— 抓包分析请直接用 protocolSelfTest()/FrameParser 自行喂数据。

// namespace JQFoundation
export const JQFoundation = {
    // bool singleApplication(const QString &flag)
    singleApplication: function (flag)
    {
        // static QSharedMemory *shareMem = nullptr;
        if (JQFoundation._shareMem)
        {
            return true;
        }

        JQFoundation._shareMem = new QSharedMemory("JQFoundationSingleApplication_" + flag);

        for (let count = 0; count < 2; ++count)
        {
            if (JQFoundation._shareMem.attach(QSharedMemory.ReadOnly))
            {
                JQFoundation._shareMem.detach();
            }
        }

        if (JQFoundation._shareMem.create(1))
        {
            return true;
        }

        return false;
    },

    _shareMem: null,
};

// QTranslator *qTranslator;
export let qTranslator = null;

// void switchAppLang(int lang)
export function switchAppLang(lang)
{
    const str = QApplication.applicationDirPath() + "/lang/bteserver_en.qm";
    qApp.removeTranslator(qTranslator);
    if (lang === 1)
    {
        if (qTranslator.load(str))
            qApp.installTranslator(qTranslator);
    }
}

// 可选的动态 import: 仓库里尚未移植的模块(CoreDump/SecurityDLL/securityclient)
// 找不到时只记账, 不再让整个 main() 挂掉。
export async function optionalImport(path)
{
    try
    {
        return await import(path);
    }
    catch (e)
    {
        console.error("[main] 可选模块未移植, 已跳过: " + path + " (" + e + ")");
        return null;
    }
}

// int main(int argc, char *argv[])
export async function main(argc, argv)
{
    const a = new QApplication(argc, argv);
    qTranslator = new QTranslator(a);

    // 以下模块在翻译层尚未补齐, 用动态 import 延后加载(见文件头说明):
    const coreDump = await optionalImport('./CoreDump.js');                  // 未移植(按要求跳过)
    const netPackDefine = await optionalImport('./netPackDefine.js');
    const server = await optionalImport('./BTEServer.js');
    await optionalImport('./SecurityDLL.js');                                // 未移植(按要求跳过)
    await optionalImport('./securityclient.js');                             // 未移植(按要求跳过)

    if (coreDump && coreDump.CoreDump)
    {
        coreDump.CoreDump.init();                        // 未移植(按要求跳过)
        coreDump.CoreDump.setCallBack(exception_exit);
    }
    qtLogHandler = qInstallMessageHandler(myMessageOutput);

    const set = new QSettings(qApp.applicationDirPath() + "/option.ini", QSettings.IniFormat);
    // Qt: set.value("lang", 0).toInt();  JS 侧 QSettings::value 返回原始值(见 qtcompat 说明)
    const lang = Number(set.value("lang", 0));
    if (lang === 1)
    {
        switchAppLang(lang);
    }

    // 注册数据包
    if (netPackDefine && netPackDefine.NetPackRegist)
        netPackDefine.NetPackRegist();
    // LogManager.instance().init(QApplication.applicationDirPath() + "/log");  // 日志由引擎 console 接管
    // LogManager.instance().setLogLevel(LOG_WARNING);

    if (!JQFoundation.singleApplication("myApplication"))
    {
        QMessageBox.information(null, "", qApp.translate("main", "程序不能重复启动"));
        return 1;
    }

    // #if 0
    //     InitDLL("SecurityXqm");
    //     int ret = -1;
    //
    //     int day;
    //     ret = CheckRegTime(day);
    //     if (ret != SECURITY_OK)
    //     {
    //         if (ret == SECURITY_OUTDAY)
    //             QMessageBox::information(NULL, "", qApp->translate("main","已过期"));
    //
    //         SecurityClient sw;
    //         if (sw.exec() != SecurityClient::Accepted)
    //             return 1;
    //     }
    // #endif

    if (!server || !server.BTEServer)
    {
        // BTEServer.js 还没能加载(它的依赖链里有尚未收口的界面层), 无法起界面
        console.error("[main] BTEServer.js 未能加载, 只启动后台服务(见 startHeadlessServer)");
        return 1;
    }

    const w = new server.BTEServer();
    w.setWindowFlags(Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint | Qt.WindowMinimizeButtonHint);
    w.showMaximized();
    w.show();
    g_server = w;

    return a.exec();
}

// ---------------------------------------------------------------------------
// 无界面入口 —— "BTE 服务端在 JS 引擎里跑起来"的最小闭环
//
// 依赖只有: 协议层(protocol.js/structs.js) + 数据包注册(netPackDefine.js)
//           + 传输层(netServer.js, 走引擎 net 模块)。
// 界面层(BTEServer.js 的 Ui_BTEServerClass/QTableWidget/...)不参与, 所以这条路径
// 现在就能在引擎里跑; 收到下位机的帧会按命令字解包并打印。
//
// 用法:
//     import { startHeadlessServer } from './main.js';
//     const h = await startHeadlessServer(8001);
//     ... h.stop();
// ---------------------------------------------------------------------------
export async function startHeadlessServer(port)
{
    const packDefine = await optionalImport('./netPackDefine.js');
    const netServerMod = await optionalImport('./netServer.js');
    if (!packDefine || !netServerMod)
    {
        console.error("[main] startHeadlessServer: 依赖模块加载失败");
        return null;
    }

    packDefine.NetPackRegist();   // 注册 NetPackVariant, 否则 NetDataManager 建不出包

    const server = new netServerMod.NetServer(null);
    server.listenError.push((msg) => console.error("[bte] listen failed: " + msg));
    server.newConnect.push((netId) => console.log("[bte] new connect netId=" + netId));
    server.disConnect.push((netId) => console.log("[bte] disconnect netId=" + netId));
    server.newDataRecv.push((netId, ptr) =>
    {
        // 与 BTEServer::onDataRecv 的分派口径一致: 用 ptr.info.CmdID 查命令
        const cmd = ptr.info.CmdID;
        const r = unpackBody(cmd, ptr.data);
        console.log("[bte] recv netId=" + netId + " sub=" + ptr.info.SubDevID +
            " cmd=" + cmdName(cmd) + " name=" + r.name);
    });

    const ok = server.startServer(port | 0);
    console.log("[bte] headless server listen " + (port | 0) + " -> " + ok);

    return {
        server,
        port: port | 0,
        stop() { server.stopServer(); },
    };
}

// ---------------------------------------------------------------------------
// 载入本文件即跑一遍协议自测(与合并前"根目录 main.js"作为测试入口的行为一致,
// 输出末尾带 BTE_SELFTEST 契约)。只做协议自测的脚本也可以直接:
//     import { protocolSelfTest } from './main.js';  protocolSelfTest();
// 不想在启动界面时跑自测, 把下面开关改成 false。
// ---------------------------------------------------------------------------
const RUN_PROTOCOL_SELFTEST = true;
if (RUN_PROTOCOL_SELFTEST)
    protocolSelfTest();
