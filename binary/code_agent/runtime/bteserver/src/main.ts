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
// 2026-09-14 变更(去 Qt 兼容层):
//   1) 本文件不再 import './qtcompat.js': 应用对象用全局 **jzApp**, 配置用 **JZSettings**,
//      弹窗用 **JZMessageBox**, 窗口/控件用真 **JZ*** —— 原来的 QApplication / QTranslator /
//      QSettings / QMessageBox / Qt / QSharedMemory 全部去掉。
//   2) 界面层(BTEServer.js)仍没收口时**不再直接返回 1**: 改为起一个真窗口外壳
//      (buildMainWindow: 真 JZWindow + 真布局 + 真控件), headless 路径保留为 startHeadlessServer()。
//   3) 本模块**被作为入口脚本加载**(引擎的入口路径就是 main.js)时自动跑 main();
//      被别的入口 import(如测试的引导脚本)时只导出, 不自动起界面 —— 判据见文件末尾 autoStart()。
//
// 历史(2026-09-13): CoreDump.js / SecurityDLL.js / securityclient.js 三个文件在仓库里根本
//   不存在(按要求未移植), 故用 optionalImport() 兜住"模块找不到"。
//   原 LogManager.js 已删除: LogInfo/LogError/LogFatal -> console.log/error, 由引擎 console 接管。
// ---------------------------------------------------------------------------

import { CMD, FrameParser, encodeFrame, decodeFrame, hexOf, cmdName, channelMask, maskChannels } from './protocol.js';
import {
    packStart70, packStop, packResume, packStepJump, packDFDataResp, packSetRtc,
    unpackBody, unpackRTData76, unpackDFData76, unpackSendInfo, packRTData76, packDFData76,
    workStatusText, isChannelFault, aTimeToISO, SIZE,
} from './structs.js';
// 本文件已**不再依赖 Qt 兼容层**: 应用对象用全局 jzApp, 配置用 JZSettings, 弹窗用 JZMessageBox,
// 窗口/控件用 JZ*(全是引擎的真实实现)。其余文件仍在迁移中 —— 见 main() 末尾的 TODO。

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
    if (g_server)
        g_server.CrashExit();
    JZMessageBox.information(null, "", "程序异常，请联系工程师");
}

// void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
// 迁移后 Qt 的消息处理器不存在了: 日志统一走引擎 console(原 qtLogHandler / qInstallMessageHandler
// 那套一并去掉)。保留函数名是因为它在原 C++ 里是导出符号, 便于对照。
export function myMessageOutput(type, context, msg)
{
    console.error("[qt] " + msg);
}

// void parseDataTest();
//   原 main.cpp 声明的自由函数, 实现文件(ParesDataTest.js, 离线抓包解析自测)已按要求删除,
//   故此处的调用点一并去掉 —— 抓包分析请直接用 protocolSelfTest()/FrameParser 自行喂数据。

// namespace JQFoundation
export const JQFoundation = {
    // bool singleApplication(const QString &flag)
    // 原实现走 QSharedMemory(共享内存)判重; 引擎没有对应原语 —— 暂时恒返回 true
    // (与迁移前 qtcompat 那个替身的行为一致)。要真做就落一个锁文件。
    singleApplication: function (flag)
    {
        return true;
    },
};

// QTranslator *qTranslator;
export let qTranslator = null;

// void switchAppLang(int lang)
// 翻译层(.qm / QTranslator)尚未移植 —— 语言切换暂时只记账, 界面文本仍是中文。
export function switchAppLang(lang)
{
    console.log("[main] switchAppLang(" + lang + "): 翻译层未移植, 界面文本仍为中文");
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
    // 应用对象由宿主创建(C++ 的 JZApplication), JS 侧通过全局 jzApp 访问其路径/退出入口
    // (见 JZJSWidgetModule::registApp)。原 `new QApplication(argc, argv)` / QTranslator 一并去掉。

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

    // 配置: 原来是 QSettings(option.ini), 现在是引擎的 JZSettings(接口名对齐, value() -> getInt())
    const set = new JZSettings();
    set.open(jzApp.applicationDirPath() + "/option.ini");
    const lang = set.getInt("lang");
    if (lang === 1)
    {
        switchAppLang(lang);
    }

    // 注册数据包
    if (netPackDefine && netPackDefine.NetPackRegist)
        netPackDefine.NetPackRegist();
    // LogManager.instance().init(...): 日志由引擎 console 接管

    if (!JQFoundation.singleApplication("myApplication"))
    {
        JZMessageBox.information(null, "", "程序不能重复启动");
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

    if (server && server.BTEServer)
    {
        // 真 BTEServer 的界面层还在移植(它的构造会碰尚未收口的 ui_*): 起得来就用它,
        // 起不来就退回下面的真窗口外壳 —— **不能**因为界面没就绪就让整个程序起不来。
        try
        {
            const w = new server.BTEServer();
            w.show();
            g_server = w;
            return 0;
        }
        catch (e)
        {
            // ⚠ 这里**别去点 e.message / e.stack**: 引擎抛出的异常对象上访问属性有可能再抛一次错,
            // 那时整个 catch 会往外抛 ⇒ main() 的 Promise 被拒绝, 表现成"**无栈**的 not a function",
            // 反而把真正的失败原因盖掉。直接把 e 交给 console.error(多参形态)。
            console.error("[main] BTEServer 起界面失败(界面层仍在移植), 退回真窗口外壳:", e);
        }
    }

    // 界面层还没收口: 先起一个**真窗口外壳** —— 真 JZWindow + 真布局 + 真控件,
    // 用的就是界面层将来要用的那套 API。
    //
    // 进度(2026-09-14): qtcompat.ts 那个 Qt 运行期替身**已删除**, 业务层全部换成引擎真实现;
    //   BTEServer 构造走到 `new StepDataDialog(this)` 才停(它还是 `QDialog.call(this)` +
    //   `new Ui_StepDataDialogClass()` 的老写法, 且依赖未移植的 qcustomplot)。
    // TODO(界面层迁移): 把 StepDataDialog + 7 个 *Dialog.ts 逐个换成"真 JZDialog + uiLoader 造的 UI"
    //   (BTEServer.ts 已经改完, 见其构造里 this.ui = createUi()), 之后这里改回 `new BTEServer()`。
    const w = buildMainWindow();
    w.show();
    console.log("[main] BTEServer 界面层仍在移植中, 已启动真窗口外壳(网络/协议层可用)");
    return 0;   // 事件循环由宿主托管(原 Qt 的 a.exec() 不需要在 JS 里跑)
}

// 主窗口外壳: 真控件搭出来的最小可跑骨架(标题 + 状态条 + 设备表)。
// 说明: 这里是**真 JZ 控件**, 不是替身 —— 界面层移植完成后这个函数会被 new BTEServer() 取代。
function buildMainWindow()
{
    const win = new JZWindow();
    win.setTitle("BTEServer");
    win.setGeometry(80, 60, 1000, 640);

    const lay = new JZVBoxLayout();

    const tip = new JZLabel("BTEServer —— 界面层移植中(当前为真控件外壳); 网络/协议层已可用");
    lay.addWidget(tip, 0);

    const table = new JZTableWidget();
    table.setColumnCount(3);
    table.setHorizontalHeaderLabels(["设备", "状态", "IP"]);
    table.setRowCount(0);
    lay.addWidget(table, 1);

    win.setLayout(lay);
    return win;
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
// 自动启动: 只有当本模块**就是入口脚本**时才起界面。
// 判据 = 引擎的入口脚本路径(__jz.core.appFile(), 见 JZEngineRuntime::runModule):
//   * 直接跑 bteserver(入口是 main.js)      -> 起窗口(buildMainWindow 或真 BTEServer);
//   * 被别的入口 import(如测试的引导脚本)   -> 只导出, 不自动起界面。
// ---------------------------------------------------------------------------
(function autoStart()
{
    try
    {
        const core = (typeof __jz !== "undefined" && __jz) ? __jz.core : null;
        const appFile = (core && core.appFile) ? String(core.appFile() || "") : "";
        if (/[\\/]main\.(js|mjs)$/i.test(appFile))
        {
            main(0, []).catch(function (e)
            {
                console.error("[main] 启动失败: " + e);
            });
        }
    }
    catch (e)
    {
        console.error("[main] autoStart 失败: " + e);
    }
})();
