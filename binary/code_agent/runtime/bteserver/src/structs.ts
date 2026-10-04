// BTE 各命令的 pack / unpack —— 逐字节对照 JZBTEServer/common/PackDefine.cpp
//
// 约定: 所有函数只处理**帧体(body)**, 不含同步头/包头/校验和/尾。
//       SubDeviceId 不进 body —— 它写在帧头的 SubDevID 字段里(见 BTEServer::send)。
//
// 注意 QDataStream 的宽度: uchar=1, ushort/short=2, int/uint=4, qint64/int64_t=8, 全部小端。
//
// 未移植(见文件末 TODO): 0x0200 SET_STEP76_Ex 工步下发(StepInfo 的参数位图/参数表)。

import { ByteReader, ByteWriter, CMD, channelMask, maskChannels, hexOf } from './protocol.js';
// 工步包 pack/unpack 要做单位换算(与 C++ PackDefine.cpp 里用 DataTrans::toXxxUnit 一致)
import { DataTrans } from './DataTrans.js';
// 工步参数下标(权威定义在 PackDefine.js): 工步包的参数位图/参数值表按它们索引
import {
    PARAM_Current, PARAM_Voltage, PARAM_Time, PARAM_Capacity, PARAM_Tempature,
    PARAM_StopCurrent, PARAM_StopVoltage, PARAM_BeginStepID, PARAM_CountLoop,
    PARAM_ProtectVoltageUpper, PARAM_ProtectVoltageLower,
    PARAM_ProtectCurrentUpper, PARAM_ProtectCurrentLower,
    PARAM_ProtectTempatureUpper, PARAM_ProtectTempatureLower,
    PARAM_ProtectDelayTime, PARAM_ProtectDetalVol, PARAM_ProtectDetalVolTime,
    PARAM_RecordTimeInterval, PARAM_RecordVoltageChange, PARAM_RecordCurrentChange,
    // 工步包 pack/unpack 的类型判定要用(C++ StepInfo::isVolt/isCurrent/... 里引用的下标)
    PARAM_ReservedParam1, PARAM_ReservedParam2, PARAM_JumpLine, PARAM_VoltageChange,
    PARAM_RecordTempatureChange,
    // 各命令包的 type() 用(下面 `export * from` 只做转发, 不建本地绑定, 所以这里要显式 import)
    BTS_CMD_START70, BTS_CMD_STOP, BTS_CMD_RESUME, BTS_CMD_STEP_JUMP,
    BTS_CMD_RESUME_CONFIRM, BTS_CMD_SET_RTC, BGS66_CMD_SET_RT_INTERVAL,
    BTS_CMD_SEND_PARALLEL, BTS_CMD_SEND_PARALLEL_CANCEL, BTS_CMD_SET_STEP76_Ex,
    OP_EQU,
} from './PackDefine.js';

// C++ 侧 PackDefine.h 把"命令字/参数枚举/错误码/PackDefine 类"和"各 stru_* 命令包"
// 放在同一个头文件里, 所以 `#include "PackDefine.h"` 能同时拿到两者; JS 拆成了
// PackDefine.js + structs.js 两个模块, 而 BTEItemWidget.js / StepDataDialog.js /
// StepSettingDialog.js 是"从 ./structs.js 拿 PackDefine/STEP_TYPE_*/PARAM_*"的写法
// (逐行翻译保留了原来的 include 语义)。这里把它们再导出一次对齐, 免得改业务文件。
export * from './PackDefine.js';

// 各结构的字节数(用于长度自检)
export const SIZE = {
    SEND_INFO: 108,
    RTDATA76: 75,
    DFDATA_DATA: 83,
    START70: 25,
    STOP: 8,
    RESUME: 8,
    STEP_JUMP: 9,
    RESUME_CONFIRM: 1,
    DFDATA_RESP76: 4,
    SET_RTC: 6,
    RT_INTERVAL: 1,
    SET_STEP_EX_RESP76: 27,
    SEND_LOG: 22,
    RTDEVSTATUS: 8,
};

// ---------------------------------------------------------------------------
// CRC32 —— 逐字对照 PackDefine.cpp 的 getCRC32():
//   反射表(标准多项式 0xEDB88320) + **初值 0** + 末尾 `crc ^ 0xFFFFFFFF`。
//   ⚠ 与 zlib/标准 CRC-32(初值 0xFFFFFFFF、末尾不再取反)写法不同, 数值也不一样 ——
//   工步包的 CRC 必须与下位机逐字节一致, 所以照抄 C++ 的算法而不是"标准写法"。
// ---------------------------------------------------------------------------
let _crc32Table = null;
function crc32Table()
{
    if (!_crc32Table)
    {
        _crc32Table = new Uint32Array(256);
        for (let i = 0; i < 256; i++)
        {
            let c = i;
            for (let k = 0; k < 8; k++)
                c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
            _crc32Table[i] = c >>> 0;
        }
    }
    return _crc32Table;
}

export function crc32(buf)
{
    if (!buf || buf.length < 1)
        return 0xffffffff;          // 与 C++ 的空输入返回一致
    const t = crc32Table();
    let crc = 0;
    for (let i = 0; i < buf.length; i++)
        crc = (t[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8)) >>> 0;
    return (crc ^ 0xffffffff) >>> 0;
}

// ---------------------------------------------------------------------------
// 设备信息 (0x72, 108 字节)
// ---------------------------------------------------------------------------
export function unpackSendInfo(body) {
    const r = new ByteReader(body);
    const o: any = {};
    o.version = r.u16();
    o.fixed = r.bytes(6);
    o.type = r.u8v();
    o.dataEndFlag = r.u8v();
    o.dataIndex = r.u8v();
    o.deviceType = r.u8v();
    o.typeNum = r.u16();
    o.channel = r.bytes(32);
    o.channelType = r.u8v();
    o.chargeMax = r.u32();
    o.dischargeMax = r.u32();
    o.voltageMax = r.u32();
    o.tempatureMax = r.u32();
    o.internalResistanceMax = r.u32();
    o.currentNum = r.u8v();
    o.currentRange = [];
    for (let i = 0; i < 10; i++) o.currentRange.push(r.u32());
    // stru_BTS_CMD_SEND_INFO::channelNum(): 数前导的 0xFF 字节, 每字节算 8 路
    let n = 0;
    for (let i = 0; i < 32; i++) { if (o.channel[i] !== 0xff) break; n += 8; }
    o.channelNum = n;
    o.deviceString = o.voltageMax + 'V' + (o.chargeMax / 1000) + 'A';  // 与 BTEServer 的拼法一致
    return o;
}

// ---------------------------------------------------------------------------
// 实时帧 (0x7C, 75 字节) —— 上位机界面/报警主要看这个
// ---------------------------------------------------------------------------
export function unpackRTData76(body) {
    const r = new ByteReader(body);
    const o: any = {};
    o.dfDataPointer = r.i32();
    o.rtDataCount = r.u8v();
    o.dataFlag = r.u8v();
    o.channelIdByNum = r.u8v();   // 通道序号, 从 1 开始
    o.auxChlIdByNum = r.u8v();    // 辅助通道序号, 主通道为 0
    o.cycleStatus = r.i32();
    o.stepId = r.u8v();
    o.stepType = r.u8v();
    o.workType = r.u8v();
    o.testTime = r.i64();          // ms
    o.voltage = r.i32();           // 0.1mV
    o.current = r.i32();
    o.internalResistance = r.i16();
    o.tempature = r.i16();         // 0.1℃
    o.chargeCap = r.i64();
    o.disChargeCap = r.i64();
    o.chargeEng = r.i64();
    o.disChargeEng = r.i64();
    o.curStepRange = r.i32();      // 电流量程 mA
    o.prtCode = r.i32();           // 保护码, 非保护为 0
    return o;
}

// WorkType 的两个判据(与 C++ workStatus() / BTEServer 的报警判据一致)
//   bit5..4: 00 完成 / 01 运行 / 10 用户停止 / 11 安全停止
//   bit7   : 通道故障
export function workStatusText(workType) {
    const s = workType & 0x30;
    if (s === 0x00) return '完成';
    if (s === 0x10) return '运行';
    if (s === 0x20) return '用户停止';
    return '安全停止';
}
export function isChannelFault(rt) {
    return (rt.workType & 0x80) !== 0 || (rt.workType & 0x30) === 0x30 || rt.prtCode !== 0;
}

// ---------------------------------------------------------------------------
// 记录点数据 (0x7D): DFDataNumber u8 | DFDataPointer i32 | n × Data(83)
// ---------------------------------------------------------------------------
export function unpackDFDataData(r) {
    const d: any = {};
    d.dataFlag = r.u8v();          // 0x55 平台点容量 / 0x56 / 0x57 / 0x58 工步终止 / 0x59 工步起始 / 0xAA LOG / 0x6A 上传完成 / 0xBB DCR
    d.subDevId = r.u8v();
    d.channelIdByNum = r.u8v();
    d.auxChlIdByNum = r.u8v();
    d.testId = r.u32();
    d.dataSerial = r.i32();
    d.cycleStatus = r.i32();
    d.stepId = r.u8v();
    d.stepType = r.u8v();
    d.stepChangeCount = r.u8v();
    d.workType = r.u8v();
    d.testTime = r.i64();
    d.voltage = r.i32();
    d.current = r.i32();
    d.internalResistance = r.i16();
    d.tempature = r.i16();
    d.chargeCap = r.i64();
    d.disChargeCap = r.i64();
    d.chargeEng = r.i64();
    d.disChargeEng = r.i64();
    d.aTime = {
        year: r.u16(),
        month: r.u8v(),
        day: r.u8v(),
        hour: r.u8v(),
        minute: r.u8v(),
        second: r.u8v(),
    };
    d.curStepRange = r.i32();
    return d;
}

export function unpackDFData76(body) {
    const r = new ByteReader(body);
    const o: any = {};
    o.dfDataNumber = r.u8v();
    o.dfDataPointer = r.i32();
    o.dataList = [];
    for (let i = 0; i < o.dfDataNumber; i++)
        o.dataList.push(unpackDFDataData(r));
    return o;
}

// ATime_t::toDateTime() —— btHour 可能 >24, 要按天进位
export function aTimeToISO(a) {
    const pad = (v, n) => String(v).padStart(n, '0');
    const extraDays = Math.floor(a.hour / 24);
    const hour = a.hour % 24;
    const ms = Date.UTC(a.year, a.month - 1, a.day + extraDays, hour, a.minute, a.second);
    const dt = new Date(ms);
    return dt.getUTCFullYear() + '-' + pad(dt.getUTCMonth() + 1, 2) + '-' + pad(dt.getUTCDate(), 2) +
        ' ' + pad(dt.getUTCHours(), 2) + ':' + pad(dt.getUTCMinutes(), 2) + ':' + pad(dt.getUTCSeconds(), 2);
}

// ATime_t::toMSec() —— 同上按天进位后转 epoch 毫秒。
// (C++ 侧是 ATime_t 的成员函数; 本移植的 aTime 是纯数据对象(协议解包/存档反序列化都产它),
//  所以做成自由函数 —— 原来业务代码里写的 `.ATime.toMSec()` 是照搬 C++ 的调用形态,
//  aTime 上根本没有这个方法, 那几处调用会直接抛 "toMSec is not a function"。)
export function aTimeToMSec(a) {
    if (!a)
        return 0;
    const extraDays = Math.floor(a.hour / 24);
    const hour = a.hour % 24;
    return Date.UTC(a.year, a.month - 1, a.day + extraDays, hour, a.minute, a.second);
}

// ---------------------------------------------------------------------------
// 下位机上报的其余几种
// ---------------------------------------------------------------------------
// 中位机日志 (0x0F)
export function unpackSendLog(body) {
    const r = new ByteReader(body);
    const o: any = {};
    o.logNumber = r.u8v();
    o.logIndex = r.u16();
    o.logSerial = r.u16();
    o.unitId = r.u8v();
    o.channelIdByNum = r.u8v();
    o.testId = r.i32();
    o.year = r.u8v() + 2000;
    o.month = r.u8v();
    o.day = r.u8v();
    o.hour = r.u8v();
    o.minute = r.u8v();
    o.second = r.u8v();
    o.logId = r.u8v();
    o.logInfo = r.i32();
    return o;
}

// 实时设备状态 (0x44, 8 字节)
export function unpackRTDevStatus(body) {
    const r = new ByteReader(body);
    const b1 = r.u8v();  // 报警状态字: bit0 温度/bit1 烟雾/bit2 气缸压力/bit3 气缸机械/bit4 无Tray/bit5 托盘倾斜/bit6 手动/bit7 急停
    const b2 = r.u8v();  // 动作状态字: bit0 针盘做接触测试
    const b3 = r.u8v();  // 温度报警 1..8
    const b4 = r.u8v();  // 温度报警 9
    const b5 = r.u8v();  // 位置状态字
    const b6 = r.u8v();  // 动作状态字
    r.skip(2);
    return { bit1: b1, bit2: b2, bit3: b3, bit4: b4, bit5: b5, bit6: b6 };
}

// 工步下发回应 (0x8200, 27 字节)
export function unpackStepExResp76(body) {
    const r = new ByteReader(body);
    const guid = r.bytes(16);
    const mask = r.bytes(8);
    return {
        guid,
        mask,
        channels: maskChannels(mask),
        dataEndFlag: r.u8v(),   // 0x01 还有后续包 / 0x02 传完
        dataIndex: r.u16(),
    };
}

// 启动回应 (0xEE): Guid(16) + ChannelCount(1) + n × (ChannelIDByNum u8 + TestID i32)
export function unpackStartResp70(body) {
    const r = new ByteReader(body);
    const o: any = {};
    o.guid = r.bytes(16);
    o.channelCount = r.u8v();
    o.channelInfo = [];
    for (let i = 0; i < o.channelCount; i++)
        o.channelInfo.push({ channelIdByNum: r.u8v(), testId: r.i32() });
    return o;
}

// 停止/接续回应 (0x91 / 0x94) 与工步跳转回应 (0xEF): 都是 8 字节通道位图
export function unpackChannelResp(body) {
    const mask = body.slice(0, 8);
    return { mask, channels: maskChannels(mask) };
}

// ---------------------------------------------------------------------------
// 上位机下发(组包)
// ---------------------------------------------------------------------------
export function packConnect(clientType) {
    const w = new ByteWriter(1);
    w.u8v(clientType || 0);
    return w.bytes();
}

// 0x6E 启动: Guid(16) + ChannelID(8) + StepID(1)
export function packStart70(o) {
    const w = new ByteWriter(SIZE.START70);
    w.raw(o.guid || new Uint8Array(16));
    w.raw(o.mask || channelMask(o.channels || []));
    w.u8v(o.stepId & 0xff);
    return w.bytes();
}

// 0x11 停止 / 0x14 接续: ChannelID(8)
export function packStop(o) {
    return channelMask(o.channels || []);
}
export function packResume(o) {
    return channelMask(o.channels || []);
}
export function packStopMask(mask) { return new Uint8Array(mask); }

// 0x6F 工步跳转: ChannelID(8) + jumpStepId(1)
export function packStepJump(o) {
    const w = new ByteWriter(SIZE.STEP_JUMP);
    w.raw(o.mask || channelMask(o.channels || []));
    w.u8v(o.jumpStepId & 0xff);
    return w.bytes();
}

// 0x36 接续反馈: result(1)
export function packResumeConfirm(result) {
    const w = new ByteWriter(1);
    w.u8v(result & 0xff);
    return w.bytes();
}

// 0x03 设置 RTC: 年(自2000) 月 日 时 分 秒
export function packSetRtc(d) {
    const w = new ByteWriter(SIZE.SET_RTC);
    w.u8v((d.year % 100) & 0xff);
    w.u8v(d.month & 0xff).u8v(d.day & 0xff);
    w.u8v(d.hour & 0xff).u8v(d.minute & 0xff).u8v(d.second & 0xff);
    return w.bytes();
}

// 0x5E 设置实时数据上报间隔(秒)
export function packSetRtInterval(seconds) {
    const w = new ByteWriter(1);
    w.u8v(seconds & 0xff);
    return w.bytes();
}

// 0xFD DF 数据回应: Index(4)
export function packDFDataResp(index) {
    const w = new ByteWriter(SIZE.DFDATA_RESP76);
    w.i32(index);
    return w.bytes();
}

// 0x0300 并联 / 0x0400 解除并联
export function packParallel(o) {
    const ch = o.channels || [];
    const w = new ByteWriter(4 + ch.length * 2);
    w.u8v((o.subDevId + 1) & 0xff);
    w.u8v((ch.length ? ch[0] + 1 : 1) & 0xff);
    w.u16(ch.length);
    for (let i = 0; i < ch.length; i++) {
        w.u8v((o.subDevId + 1) & 0xff);
        w.u8v((ch[i] + 1) & 0xff);
    }
    return w.bytes();
}

// ---------------------------------------------------------------------------
// 上报包的组包(下位机模拟器 / 测试用; C++ 侧同样有 pack 实现)
// ---------------------------------------------------------------------------
export function packRTData76(o) {
    const w = new ByteWriter(SIZE.RTDATA76);
    w.i32(o.dfDataPointer | 0);
    w.u8v(o.rtDataCount & 0xff);
    w.u8v(o.dataFlag & 0xff);
    w.u8v(o.channelIdByNum & 0xff);
    w.u8v(o.auxChlIdByNum & 0xff);
    w.i32(o.cycleStatus | 0);
    w.u8v(o.stepId & 0xff);
    w.u8v(o.stepType & 0xff);
    w.u8v(o.workType & 0xff);
    w.i64(o.testTime || 0);
    w.i32(o.voltage | 0);
    w.i32(o.current | 0);
    w.i16(o.internalResistance | 0);
    w.i16(o.tempature | 0);
    w.i64(o.chargeCap || 0);
    w.i64(o.disChargeCap || 0);
    w.i64(o.chargeEng || 0);
    w.i64(o.disChargeEng || 0);
    w.i32(o.curStepRange | 0);
    w.i32(o.prtCode | 0);
    return w.bytes();
}

export function packDFDataData(d) {
    const w = new ByteWriter(SIZE.DFDATA_DATA);
    w.u8v(d.dataFlag & 0xff);
    w.u8v(d.subDevId & 0xff);
    w.u8v(d.channelIdByNum & 0xff);
    w.u8v(d.auxChlIdByNum & 0xff);
    w.u32(d.testId || 0);
    w.i32(d.dataSerial | 0);
    w.i32(d.cycleStatus | 0);
    w.u8v(d.stepId & 0xff);
    w.u8v(d.stepType & 0xff);
    w.u8v(d.stepChangeCount & 0xff);
    w.u8v(d.workType & 0xff);
    w.i64(d.testTime || 0);
    w.i32(d.voltage | 0);
    w.i32(d.current | 0);
    w.i16(d.internalResistance | 0);
    w.i16(d.tempature | 0);
    w.i64(d.chargeCap || 0);
    w.i64(d.disChargeCap || 0);
    w.i64(d.chargeEng || 0);
    w.i64(d.disChargeEng || 0);
    const a = d.aTime || {};
    w.u16(a.year || 0);
    w.u8v(a.month || 0).u8v(a.day || 0);
    w.u8v(a.hour || 0).u8v(a.minute || 0).u8v(a.second || 0);
    w.i32(d.curStepRange | 0);
    return w.bytes();
}

export function packDFData76(o) {
    const list = o.dataList || [];
    const w = new ByteWriter(1 + 4 + list.length * SIZE.DFDATA_DATA);
    w.u8v(list.length & 0xff);
    w.i32(o.dfDataPointer | 0);
    for (let i = 0; i < list.length; i++) w.raw(packDFDataData(list[i]));
    return w.bytes();
}

// ---------------------------------------------------------------------------
// 按命令字分派解包(下位机 -> 上位机)
// ---------------------------------------------------------------------------
export function unpackBody(cmdId, body) {
    switch (cmdId) {
        case CMD.SEND_INFO: return { name: 'SEND_INFO', value: unpackSendInfo(body) };
        case CMD.SEND_RTDATA76: return { name: 'RTDATA76', value: unpackRTData76(body) };
        case CMD.SEND_DFDATA76: return { name: 'DFDATA76', value: unpackDFData76(body) };
        case CMD.SEND_LOG: return { name: 'SEND_LOG', value: unpackSendLog(body) };
        case CMD.SEND_RTDEVSTATUS: return { name: 'RTDEVSTATUS', value: unpackRTDevStatus(body) };
        case CMD.SET_STEP_EX_RESP76: return { name: 'SET_STEP_EX_RESP76', value: unpackStepExResp76(body) };
        case CMD.START_RESP70: return { name: 'START_RESP70', value: unpackStartResp70(body) };
        case CMD.STOP_RESP:
        case CMD.RESUME_RESP:
        case CMD.STEP_JUMP_RESP: return { name: 'CHANNEL_RESP', value: unpackChannelResp(body) };
        case CMD.RESUME_QUERY: return { name: 'RESUME_QUERY', value: {} };
        case CMD.RESUME_CONFIRM_RESP: return { name: 'RESUME_CONFIRM_RESP', value: { result: body.length > 0 ? body[0] : 0 } };
        case CMD.HEART_BEAT: return { name: 'HEART_BEAT', value: {} };
        case CMD.CONNECT: return { name: 'CONNECT', value: { clientType: body.length > 0 ? body[0] : 0 } };
        default: return { name: 'UNKNOWN', value: { hex: hexOf(body) } };
    }
}

// ===========================================================================
// stru_* 命令包类 —— 对应 C++ PackDefine.h 里的一组"命令包"结构体
//
// 为什么要这层: 界面/数据层代码是 C++ 逐行翻译的, 用的是**结构体 + 方法**的写法
// (`const data76 = new stru_BTS_CMD_SEND_RTDATA76(); data76.unpack(ptr.data); data76.PrtCode`),
// 而本文件前面那些 pack*/unpack* 是"函数式"的纯协议层。这里把两者对上:
//   字段名 = C++ 结构体字段名(界面代码用的就是这些名字);
//   unpack(body) / pack() = 转发到上面的函数式实现。
//
// 已收口: 上报帧(SEND_INFO/RTDATA76/DFDATA76 及其子项) + 控制帧(START70/STOP/
//         RESUME/STEP_JUMP/SET_RTC/RT_INTERVAL/PARALLEL/LED_ON) + 各回应帧。
// 未收口: 0x0200 SET_STEP76_Ex 的 **参数位图/参数表** pack/unpack(见文件末 TODO),
//         现在只给出界面/注册流程需要的类骨架与参数类型表。
// ===========================================================================

// C++ 结构体的"字段名 -> 协议层小写名"搬运
function copyIn(dst, src, map)
{
    for (const k in map)
        if (src[map[k]] !== undefined)
            dst[k] = src[map[k]];
}
function copyOut(src, map, out)
{
    for (const k in map)
        out[map[k]] = src[k];
    return out;
}

// ---------------------------------------------------------------------------
// stru_BTS_CMD_SEND_INFO (0x72, 108 字节)
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_SEND_INFO
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.Version = 0;
        this.Fixed = new Uint8Array(6);
        this.Type = 0;
        this.DataEndFlag = 0;
        this.DataIndex = 0;
        this.DeviceType = 0;
        this.TypeNum = 0;
        this.Channel = new Uint8Array(32);
        this.ChannelType = 0;
        this.ChargeMax = 0;
        this.DischargeMax = 0;
        this.VoltageMax = 0;
        this.TempatureMax = 0;
        this.InternalResistanceMax = 0;
        this.CurrentNum = 0;
        this.CurrentRange = new Array(10).fill(0);
        this.ChannelNum = 0;
        this.DeviceString = "";
    }
    unpack(body)
    {
        const o = unpackSendInfo(body);
        copyIn(this, o, {
            Version: "version", Fixed: "fixed", Type: "type", DataEndFlag: "dataEndFlag",
            DataIndex: "dataIndex", DeviceType: "deviceType", TypeNum: "typeNum",
            Channel: "channel", ChannelType: "channelType", ChargeMax: "chargeMax",
            DischargeMax: "dischargeMax", VoltageMax: "voltageMax", TempatureMax: "tempatureMax",
            InternalResistanceMax: "internalResistanceMax", CurrentNum: "currentNum",
            CurrentRange: "currentRange", ChannelNum: "channelNum", DeviceString: "deviceString",
        });
        return true;
    }
    pack(buffer)
    {
        // 与 C++ 一样按字段布局写回 108 字节
        const w = new ByteWriter(SIZE.SEND_INFO);
        w.u16(this.Version);
        w.raw(this.Fixed);
        w.u8v(this.Type).u8v(this.DataEndFlag).u8v(this.DataIndex).u8v(this.DeviceType);
        w.u16(this.TypeNum);
        w.raw(this.Channel);
        w.u8v(this.ChannelType);
        w.u32(this.ChargeMax).u32(this.DischargeMax).u32(this.VoltageMax);
        w.u32(this.TempatureMax).u32(this.InternalResistanceMax);
        w.u8v(this.CurrentNum);
        for (let i = 0; i < 10; i++) w.u32(this.CurrentRange[i] || 0);
        const b = w.bytes();
        if (buffer) buffer.push(b);
        return b;
    }
    channelNum() { return this.ChannelNum; }
}

// ---------------------------------------------------------------------------
// stru_BTS_CMD_SEND_RTDATA76 (0x7C, 75 字节) —— 界面/报警的核心上报帧
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_SEND_RTDATA76
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // enum WorkStatus { WORK_COMPLETE, WORK_RUN, WORK_USER_STOP, WORK_ERROR_STOP };
    static get WORK_COMPLETE() { return 0; }
    static get WORK_RUN() { return 1; }
    static get WORK_USER_STOP() { return 2; }
    static get WORK_ERROR_STOP() { return 3; }

    constructor()
    {
        this.DFDataPointer = 0;
        this.RTDataCount = 0;
        this.DataFlag = 0;
        this.ChannelIDByNum = 1;
        this.AuxChlIDByNum = 0;
        this.CycleStatus = 0;
        this.StepID = 0;
        this.StepType = 0;
        this.WorkType = 0;
        this.TestTime = 0n;
        this.Voltage = 0;
        this.Current = 0;
        this.InternalResistance = 0;
        this.Tempature = 0;
        this.ChargeCap = 0n;
        this.DisChargeCap = 0n;
        this.ChargeEng = 0n;
        this.DisChargeEng = 0n;
        this.CurStepRange = 0;
        this.PrtCode = 0;
    }
    // WorkType bit5..4: 00 完成 / 01 运行 / 10 用户停止 / 11 安全停止(按报错处理)
    workStatus()
    {
        return (this.WorkType & 0x30) >> 4;
    }
    unpack(body)
    {
        const o = unpackRTData76(body);
        copyIn(this, o, {
            DFDataPointer: "dfDataPointer", RTDataCount: "rtDataCount", DataFlag: "dataFlag",
            ChannelIDByNum: "channelIdByNum", AuxChlIDByNum: "auxChlIdByNum",
            CycleStatus: "cycleStatus", StepID: "stepId", StepType: "stepType",
            WorkType: "workType", TestTime: "testTime", Voltage: "voltage", Current: "current",
            InternalResistance: "internalResistance", Tempature: "tempature",
            ChargeCap: "chargeCap", DisChargeCap: "disChargeCap",
            ChargeEng: "chargeEng", DisChargeEng: "disChargeEng",
            CurStepRange: "curStepRange", PrtCode: "prtCode",
        });
        return true;
    }
    pack(buffer)
    {
        const b = packRTData76(copyOut(this, {
            DFDataPointer: "dfDataPointer", RTDataCount: "rtDataCount", DataFlag: "dataFlag",
            ChannelIDByNum: "channelIdByNum", AuxChlIDByNum: "auxChlIdByNum",
            CycleStatus: "cycleStatus", StepID: "stepId", StepType: "stepType",
            WorkType: "workType", TestTime: "testTime", Voltage: "voltage", Current: "current",
            InternalResistance: "internalResistance", Tempature: "tempature",
            ChargeCap: "chargeCap", DisChargeCap: "disChargeCap",
            ChargeEng: "chargeEng", DisChargeEng: "disChargeEng",
            CurStepRange: "curStepRange", PrtCode: "prtCode",
        }, {}));
        if (buffer) buffer.push(b);
        return b;
    }
}

// ---------------------------------------------------------------------------
// stru_BTS_CMD_SEND_DFDATA76 (0x7D): 记录点数据 + 内嵌的结构体 Data
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_SEND_DFDATA76
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // 对应 C++ 的 stru_BTS_CMD_SEND_DFDATA76::Data (83 字节)
    static get Data() { return stru_BTS_CMD_SEND_DFDATA76_Data; }

    constructor()
    {
        this.DFDataNumber = 0;   // 个数
        this.DFDataPointer = 0;  // 数据存储指针
        this.dataList = [];      // QVector<Data>
    }
    unpack(body)
    {
        const o = unpackDFData76(body);
        this.DFDataNumber = o.dfDataNumber;
        this.DFDataPointer = o.dfDataPointer;
        this.dataList = [];
        for (let i = 0; i < o.dataList.length; i++)
        {
            const d = new stru_BTS_CMD_SEND_DFDATA76_Data();
            d._assign(o.dataList[i]);
            this.dataList.push(d);
        }
        return true;
    }
    pack(buffer)
    {
        const b = packDFData76({
            dfDataPointer: this.DFDataPointer,
            dataList: this.dataList.map((d) => d._toBody()),
        });
        if (buffer) buffer.push(b);
        return b;
    }
}

const DFDATA_FIELD_MAP = {
    DataFlag: "dataFlag", SubDevID: "subDevId", ChannelIDByNum: "channelIdByNum",
    AuxChlIDByNum: "auxChlIdByNum", TestID: "testId", DataSerial: "dataSerial",
    CycleStatus: "cycleStatus", StepID: "stepId", StepType: "stepType",
    StepChangeCount: "stepChangeCount", WorkType: "workType", TestTime: "testTime",
    Voltage: "voltage", Current: "current", InternalResistance: "internalResistance",
    Tempature: "tempature", ChargeCap: "chargeCap", DisChargeCap: "disChargeCap",
    ChargeEng: "chargeEng", DisChargeEng: "disChargeEng", ATime: "aTime",
    CurStepRange: "curStepRange",
};

export class stru_BTS_CMD_SEND_DFDATA76_Data
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.DataFlag = 0;
        this.SubDevID = 1;
        this.ChannelIDByNum = 1;
        this.AuxChlIDByNum = 0;
        this.TestID = 0;
        this.DataSerial = 0;
        this.CycleStatus = 0;
        this.StepID = 0;
        this.StepType = 0;
        this.StepChangeCount = 0;
        this.WorkType = 0;
        this.TestTime = 0n;
        this.Voltage = 0;
        this.Current = 0;
        this.InternalResistance = 0;
        this.Tempature = 0;
        this.ChargeCap = 0n;
        this.DischargeCap = 0n;
        this.DisChargeCap = 0n;
        this.ChargeEng = 0n;
        this.DisChargeEng = 0n;
        this.ATime = { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
        this.CurStepRange = 0;
    }
    _assign(o) { copyIn(this, o, DFDATA_FIELD_MAP); return this; }
    _toBody() { return copyOut(this, DFDATA_FIELD_MAP, {}); }
    unpack(body)
    {
        this._assign(unpackDFDataData(new ByteReader(body)));
        return true;
    }
    pack(buffer)
    {
        const b = packDFDataData(this._toBody());
        if (buffer) buffer.push(b);
        return b;
    }
}

// ---------------------------------------------------------------------------
// 各种回应帧 / 短命令帧
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_SEND_DFDATA_RESP76
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor() { this.Index = 0; }
    pack(buffer)
    {
        const b = packDFDataResp(this.Index);
        if (buffer) buffer.push(b);
        return b;
    }
}

// ---------------------------------------------------------------------------
// stru_BTS_CMD_STEP_REQUEST —— 工步下发队列里的包(C++ 里是 NetPack 的子类)。
// C++ 里 START70 / STOP / RESUME / RESUME_CONFIRM / STEP_JUMP / SEND_PARALLEL(CANCEL)
// / SET_RTC / SET_STEP76_Ex **全都继承它**, 所以基类必须是它(见下面 stru_ChannelPackBase)。
//
// ⚠ 2026-09-23 修正(原来是断的): 这些包以前继承的是另一个互不相干的 stru_ChannelPackBase,
//   而 BTEServer.send(netId, pack) 用 `pack instanceof stru_BTS_CMD_STEP_REQUEST` 判
//   "第 2 个参数是包还是命令字" ⇒ 判断永远为假, 启动/停止/接续/跳转/并联的发送路径整条走不通
//   (会掉进"当成命令字"的分支, 然后 data.length 报错)。同时补上各包的 type()
//   (原来只有基类有 type() 且恒返回 0x0200 —— 就算 instanceof 修好了也会把所有包都发成工步包)。
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_STEP_REQUEST
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor()
    {
        this.Guid = new Uint8Array(16);
        this.ChannelID = new Uint8Array(8);
        this.DataEndFlag = 0;
        this.DataIndex = 0;
    }
    type() { return CMD.SET_STEP76_Ex; }   // 派生类各自覆写
    // C++ stru_BTS_CMD_*::setChannel(int idx): 把**第 idx 路**对应的位**置进** ChannelID
    // (下标 0 基, 可多次调用累加多路)。注意这不是"整张位图"——createPackList 里就是
    // 逐路调用 pack.setChannel(chn_idx) 来攒出一张位图的, 传数组是另一种写法(兼容保留)。
    setChannel(idx)
    {
        if (typeof idx === 'number')
        {
            if (idx >= 0 && idx < 64)
                this.ChannelID[(idx / 8) | 0] |= (1 << (idx % 8));
        }
        else if (idx && idx.length !== undefined)
            this.ChannelID = channelMask(idx);
        return this;
    }
    channels() { return maskChannels(this.ChannelID); }
    clearChannel() { this.ChannelID = new Uint8Array(8); }
    pack(buffer)
    {
        const out = new ByteWriter(16 + 8 + 1 + 2);
        out.raw(this.Guid);
        out.raw(this.ChannelID);
        out.u8v(this.DataEndFlag);
        out.u16(this.DataIndex);
        const b = out.bytes();
        if (buffer) buffer.push(b);
        return b;
    }
    unPack(buffer)
    {
        const r = new ByteReader(buffer);
        this.Guid = r.bytes(16);
        this.ChannelID = r.bytes(8);
        this.DataEndFlag = r.u8v();
        this.DataIndex = r.u16();
        return true;
    }
}

// 带通道位图的命令包基类: START70 / STOP / RESUME / STEP_JUMP / PARALLEL(及 CANCEL)
// 都是"通道位图 + 附加字段", 与 C++ 同一条继承链(基类就是 STEP_REQUEST, 不再另立一份)。
export class stru_ChannelPackBase extends stru_BTS_CMD_STEP_REQUEST
{
}

export class stru_BTS_CMD_START70 extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    type() { return BTS_CMD_START70; }
    // C++ 侧是两个构造(无参 + 拷贝语义的 const stru_BTS_CMD_START70 &src) ⇒ JS 侧合成一个:
    // src 省略时就等价于 C++ 的无参构造(下面 if (src) 整段跳过)。
    constructor(src = null)
    {
        super();
        this.Guid = new Uint8Array(16);
        this.StepID = 1;
        if (src)
        {
            if (src.Guid) this.Guid = new Uint8Array(src.Guid);
            if (src.ChannelID) this.ChannelID = new Uint8Array(src.ChannelID);
            if (src.StepID !== undefined) this.StepID = src.StepID;
        }
    }
    pack(buffer)
    {
        const b = packStart70({ guid: this.Guid, mask: this.ChannelID, stepId: this.StepID });
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_STOP extends stru_ChannelPackBase
{
    type() { return BTS_CMD_STOP; }
    pack(buffer)
    {
        const b = new Uint8Array(this.ChannelID);   // 通道位图原样下发
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_RESUME extends stru_BTS_CMD_STOP
{
    type() { return BTS_CMD_RESUME; }
}

export class stru_BTS_CMD_STEP_JUMP extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // C++ 成员名是小写 jumpStepId(PackDefine.h:648), 调用点写的也是 pack.jumpStepId = ...
    constructor()
    {
        super();
        this.jumpStepId = 1;
    }
    type() { return BTS_CMD_STEP_JUMP; }
    pack(buffer)
    {
        const b = packStepJump({ mask: this.ChannelID, jumpStepId: this.jumpStepId });
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_RESUME_CONFIRM extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // C++ 成员名是小写 result, 且构造默认 1(PackDefine.cpp:1282-1285)。调用点写的也是
    // `confirm.result = ...` —— 之前类里叫 Result, 两边对不上(pack 会恒发默认值)。
    constructor() { super(); this.result = 1; }
    type() { return BTS_CMD_RESUME_CONFIRM; }
    pack(buffer)
    {
        const b = packResumeConfirm(this.result);
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_SET_RTC extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // C++: `QDateTime time;`(PackDefine.h:340) —— 小写 time, 调用点写的也是 rtc.time = Date。
    constructor()
    {
        super();
        this.time = null;   // Date(原 C++ 是 QDateTime); 也允许逐字段设(见下)
        this.Year = 0; this.Month = 0; this.Day = 0;
        this.Hour = 0; this.Minute = 0; this.Second = 0;
    }
    type() { return BTS_CMD_SET_RTC; }
    pack(buffer)
    {
        // C++ 只用 time(PackDefine.cpp:167-180): 年(-2000) 月 日 时 分 秒 各 1 字节。
        // 逐字段那几个是移植时留的备用口子: time 没设时用它们。
        const t = (this.time instanceof Date) ? this.time : null;
        const b = packSetRtc({
            year: t ? t.getFullYear() : this.Year,
            month: t ? t.getMonth() + 1 : this.Month,
            day: t ? t.getDate() : this.Day,
            hour: t ? t.getHours() : this.Hour,
            minute: t ? t.getMinutes() : this.Minute,
            second: t ? t.getSeconds() : this.Second,
        });
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BGS66_CMD_SET_RT_INTERVAL extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // C++ 成员名是 RTInterval(PackDefine.h:359), 调用点写的也是 RTInterval.RTInterval = 1。
    constructor() { super(); this.RTInterval = 0; }
    type() { return BGS66_CMD_SET_RT_INTERVAL; }
    pack(buffer)
    {
        const b = packSetRtInterval(this.RTInterval);
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_SEND_PARALLEL extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // C++: 基类的 SubDeviceId + `QVector<uchar> channelInfo`(PackDefine.h:686-693);
    // 调用点写的也是 pack.SubDeviceId / pack.channelInfo.push(...)。
    constructor()
    {
        super();
        this.SubDeviceId = 0;
        this.channelInfo = [];
    }
    type() { return BTS_CMD_SEND_PARALLEL; }
    pack(buffer)
    {
        const b = packParallel({
            subDevId: this.SubDeviceId,
            channels: this.channelInfo.length ? this.channelInfo : maskChannels(this.ChannelID),
        });
        if (buffer) buffer.push(b);
        return b;
    }
}

export class stru_BTS_CMD_SEND_PARALLEL_CANCEL extends stru_BTS_CMD_SEND_PARALLEL
{
    type() { return BTS_CMD_SEND_PARALLEL_CANCEL; }
}

export class stru_BTS_CMD_LED_ON
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor() { this.Led = new Uint8Array(8); }
    pack(buffer)
    {
        const w = new ByteWriter(8);
        w.raw(this.Led);
        const b = w.bytes();
        if (buffer) buffer.push(b);
        return b;
    }
}

// ---- 回应帧(只解包) ----
export class stru_BTS_CMD_START_RESP70
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor() { this.Guid = new Uint8Array(16); this.ChannelCount = 0; this.channelInfo = []; }
    unpack(body)
    {
        const o = unpackStartResp70(body);
        this.Guid = o.guid;
        this.ChannelCount = o.channelCount;
        this.channelInfo = o.channelInfo;
        return true;
    }
}

export class stru_BTS_CMD_CHANNEL_RESP
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor() { this.ChannelID = new Uint8Array(8); }
    unpack(body)
    {
        const o = unpackChannelResp(body);
        this.ChannelID = o.mask;
        return true;
    }
}
export class stru_BTS_CMD_STOP_RESP extends stru_BTS_CMD_CHANNEL_RESP { }
export class stru_BTS_CMD_RESUME_RESP extends stru_BTS_CMD_CHANNEL_RESP { }
export class stru_BTS_CMD_STEP_JUMP_RESP extends stru_BTS_CMD_CHANNEL_RESP { }

// ---------------------------------------------------------------------------
// stru_BTS_CMD_SET_STEP76_Ex (0x0200) —— 工步下发
//
// **未收口**: 参数位图 + 参数表的 pack/unpack 对应 PackDefine.cpp:435-830,
// 尚未移植(见文件末 TODO)。这里先给出界面/注册流程真正用到的骨架:
//   StepInfo.ParamInfo —— 参数类型表(TYPE_PARAM/TYPE_NONE/...)
//   StepInfo.typeList  —— 64 项参数类型(netPackDefine 初始化用)
//   StepInfo           —— 单个工步的字段占位
// ---------------------------------------------------------------------------
export class stru_BTS_CMD_SET_STEP76_Ex_StepInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明

    // ---- static 成员(原 PackDefine.h 的 enum ParamType + static 表) ----
    // ⚠ 必须在**类体里**声明: 类外的 `X.ParamInfo = ...` 只做赋值、不给类补类型(TS2339)。
    // enum ParamType { TYPE_NONE, TYPE_BITMAP, TYPE_PARAM, TYPE_RANGE, TYPE_ENUM };
    static ParamInfo = {
        TYPE_NONE: 0,
        TYPE_BITMAP: 1,
        TYPE_PARAM: 2,
        TYPE_RANGE: 3,
        TYPE_ENUM: 4,
    };
    // 64 项参数类型表(netPackDefine 初始化时按工步参数个数填 TYPE_PARAM, 首尾为 TYPE_NONE)
    static typeList: number[] = new Array(64).fill(2);

    constructor()
    {
        this.StepLine = new Uint8Array(8);      // 参数位图: 两个 32 位(低 4 字节 + 高 4 字节)
        this.paramList = [];                    // 参数值表, 下标 = PARAM_*(见 PackDefine.js)
        this.paramOp = [];                      // 各参数的比较运算符(OP_*), undefined = 未设置
        this.StepType = 0;
        this.StepID = 1;
        // ⚠ 名字说明: C++ 的 `ushort StepLine` 是**工步行号**(updateStepLine 里 = 工步序号 + 1),
        //   而报文里的**参数位图**在 C++ 里是 paramList[i].vaild + getBitmap()。
        //   本移植把 StepLine 这个名字用给了参数位图(界面层的 isXxxVaild/_setBit 都读它),
        //   所以工步行号另立一个字段 stepLineNo, 语义与 C++ 的 StepLine 一致。
        this.stepLineNo = 0;
    }

    // ---- 参数位图(PARAM_* 索引 -> bit) ----
    _hasBit(i) { return (this.StepLine[(i / 8) | 0] & (1 << (i % 8))) !== 0; }
    _setBit(i) { this.StepLine[(i / 8) | 0] |= (1 << (i % 8)); }
    _clearBit(i) { this.StepLine[(i / 8) | 0] &= (~(1 << (i % 8))) & 0xff; }

    // ---- 参数的统一读写 ----
    //   C++ 里"设置一个工步参数"同时做三件事: 记值 / 记比较符 / 置位图。这里同形收口。
    _get(i) { const v = this.paramList[i]; return (v === undefined) ? 0 : v; }
    _set(i, v, op = null)
    {
        this.paramList[i] = v;
        if (op !== undefined && op !== null)
            this.paramOp[i] = op;
        this._setBit(i);
        return this;
    }

    // ---- 参数是否被设置(界面据此决定输入框是否可用) ----
    isCurrentVaild() { return this._hasBit(PARAM_Current); }
    isVoltageVaild() { return this._hasBit(PARAM_Voltage); }
    isTimeVaild() { return this._hasBit(PARAM_Time); }
    isCapacityVaild() { return this._hasBit(PARAM_Capacity); }
    isTempatureVaild() { return this._hasBit(PARAM_Tempature); }
    isStopCurrentVaild() { return this._hasBit(PARAM_StopCurrent); }
    isStopVoltageVaild() { return this._hasBit(PARAM_StopVoltage); }
    isBeginStepIDVaild() { return this._hasBit(PARAM_BeginStepID); }
    isCountLoopVaild() { return this._hasBit(PARAM_CountLoop); }
    isRecordTimeIntervalVaild() { return this._hasBit(PARAM_RecordTimeInterval); }

    // ---- 取值 ----
    Current() { return this._get(PARAM_Current); }
    Voltage() { return this._get(PARAM_Voltage); }
    Time() { return this._get(PARAM_Time); }
    Capacity() { return this._get(PARAM_Capacity); }
    Tempature() { return this._get(PARAM_Tempature); }
    StopCurrent() { return this._get(PARAM_StopCurrent); }
    StopVoltage() { return this._get(PARAM_StopVoltage); }
    BeginStepID() { return this._get(PARAM_BeginStepID); }
    CountLoop() { return this._get(PARAM_CountLoop); }
    CycleStatus() { return 0; }             // 循环状态由下位机上报, 工步本身不带

    // ---- 设值(与 C++ 的 setXxx(OP_xxx, value) 同形) ----
    setCurrent(op, v) { return this._set(PARAM_Current, v, op); }
    setVoltage(op, v) { return this._set(PARAM_Voltage, v, op); }
    setTime(op, v) { return this._set(PARAM_Time, v, op); }
    setCapacity(op, v) { return this._set(PARAM_Capacity, v, op); }
    setTempature(op, v) { return this._set(PARAM_Tempature, v, op); }
    setStopCurrent(op, v) { return this._set(PARAM_StopCurrent, v, op); }
    setStopVoltage(op, v) { return this._set(PARAM_StopVoltage, v, op); }
    setBeginStepID(op, v) { return this._set(PARAM_BeginStepID, v, op); }
    setCountLoop(op, v) { return this._set(PARAM_CountLoop, v, op); }

    // 保护 / 记录类参数(界面只给值; 比较符由下位机的语义决定, 不在这里挑)
    setProtectVoltageUpper(v) { return this._set(PARAM_ProtectVoltageUpper, v); }
    setProtectVoltageLower(v) { return this._set(PARAM_ProtectVoltageLower, v); }
    setProtectCurrentUpper(v) { return this._set(PARAM_ProtectCurrentUpper, v); }
    setProtectCurrentLower(v) { return this._set(PARAM_ProtectCurrentLower, v); }
    setProtectTempatureUpper(v) { return this._set(PARAM_ProtectTempatureUpper, v); }
    setProtectTempatureLower(v) { return this._set(PARAM_ProtectTempatureLower, v); }
    setProtectDelayTime(v) { return this._set(PARAM_ProtectDelayTime, v); }
    setProtectDetalVol(v) { return this._set(PARAM_ProtectDetalVol, v); }
    setProtectDetalVolTime(v) { return this._set(PARAM_ProtectDetalVolTime, v); }
    setRecordTimeInterval(v) { return this._set(PARAM_RecordTimeInterval, v); }
    setRecordVoltageChange(v) { return this._set(PARAM_RecordVoltageChange, v); }
    setRecordCurrentChange(v) { return this._set(PARAM_RecordCurrentChange, v); }

    // 清空全部参数(C++ clearParams): 值表/运算符表/位图一起清
    clearParams()
    {
        this.paramList = [];
        this.paramOp = [];
        this.StepLine = new Uint8Array(8);
        return this;
    }

    // 由参数表重算位图(界面改过参数后调一次; _set 已经置位, 这里是兜底的"以值表为准")
    updateStepLine()
    {
        this.StepLine = new Uint8Array(8);
        for (let i = 0; i < this.paramList.length; i++)
            if (this.paramList[i] !== undefined && this.paramList[i] !== null)
                this._setBit(i);
        return this;
    }

    // 清空**单个**参数(值 + 运算符 + 位图位) —— 对应 C++ `paramList[i].clear()`
    // (原调用点写的是 `step.paramList[i].clear()`; 本移植的 paramList 是纯值数组,
    //  没有 ParamInfo 对象, 所以收口成这个方法。见 StepSettingDialog.ts 的调用点。)
    clearParam(i)
    {
        delete this.paramList[i];
        delete this.paramOp[i];
        this._clearBit(i);
        return this;
    }

    // ---- 参数类型判定(一一对应 C++ StepInfo::isReserved/isVolt/isCurrent/isTemp/
    //      isCapacity/isTime64/isInt32; 决定该参数在报文里的字节宽度) ----
    // 注意与上面那些 isXxxVaild() 区分: 这里判的是"参数**类型**", 那里判的是"有没有被设置"。
    isReserved(i) { return i === PARAM_ReservedParam1 || i === PARAM_ReservedParam2; }
    isVolt(i)
    {
        return i === PARAM_Voltage || i === PARAM_StopVoltage || i === PARAM_VoltageChange
            || i === PARAM_ProtectVoltageLower || i === PARAM_ProtectVoltageUpper
            || i === PARAM_RecordVoltageChange || i === PARAM_ProtectDetalVol;
    }
    isCurrent(i)
    {
        return i === PARAM_Current || i === PARAM_StopCurrent
            || i === PARAM_ProtectCurrentUpper || i === PARAM_ProtectCurrentLower
            || i === PARAM_RecordCurrentChange;
    }
    isTemp(i)
    {
        return i === PARAM_Tempature || i === PARAM_ProtectTempatureUpper
            || i === PARAM_ProtectTempatureLower || i === PARAM_RecordTempatureChange;
    }
    isCapacity(i) { return i === PARAM_Capacity; }
    isTime64(i) { return i === PARAM_Time || i === PARAM_RecordTimeInterval; }
    isInt32(i)
    {
        return i === PARAM_BeginStepID || i === PARAM_CountLoop || i === PARAM_JumpLine
            || i === PARAM_ProtectDelayTime || i === PARAM_ProtectDetalVolTime;
    }

    // C++ getBitmap(index): 取第 index 个 32 位位图; 非 0 时置 bit31 = "后面还有位图"。
    _bitmapOf(index)
    {
        const s = index * 32;
        let v = 0;
        for (let i = s; i < s + 32 && i < 64; i++)
            if (this._hasBit(i))
                v |= (1 << (i - s));
        if (v !== 0)
            v |= (1 << 31);
        return v >>> 0;
    }

    // C++ setBitmap(index, bitmap): 把位图回填到 paramList(⚠ C++ 那边循环上界是 s+31,
    // 即漏掉每组的最后一位 bit30 —— 这是它的原样行为, 这里照抄以保持逐字节一致)。
    setBitmap(index, bitmap)
    {
        const s = index * 32;
        const e = s + 31;
        for (let i = s; i < e && i < 64; i++)
        {
            if ((bitmap & (1 << (i - s))) !== 0)
                this._setBit(i);
            else
                this._clearBit(i);
        }
    }
    getBitmap(index) { return this._bitmapOf(index); }

    // 该参数比较运算符(C++ clearParams 把 op 全置 OP_EQU; 本移植的 paramOp 是稀疏数组,
    // 缺省同样按 OP_EQU 处理)
    _op(i)
    {
        const op = this.paramOp[i];
        return (op === undefined || op === null) ? OP_EQU : op;
    }

    // ---- C++ StepInfo::pack(PackDefine.cpp:435-507) —— 单个工步的报文 ----
    //   ushort 长度(= body + 10, 即 6 字节头 + 4 字节 crc)
    //   ushort StepLine(工步行号) | uchar StepID | uchar StepType
    //   body: bitMap[0..n-1](u32, 遇到 0 结束; bit31 = 后面还有 bitMap 的标志位)
    //         + 按位序依次排列的有效参数:
    //             预留参数: i32(无运算符)   其余: op(1B) + 数值
    //             电压/电流/温度/整型 = 4B, 容量/时间 = 8B
    //   u32 crc32(覆盖前 长度+头+body 的全部字节)
    pack(buffer)
    {
        // 1) 位图: 一直取到"某个 32 位为 0"(那一个也要写进去), 最多 12 个
        const bitMaps = [];
        for (let i = 0; i < 12; i++)
        {
            const m = this._bitmapOf(i);
            bitMaps.push(m);
            if (m === 0)
                break;
        }

        // 2) body
        // ⚠ ByteWriter.bytes() 返回的是**整个缓冲**(没写满的尾部是 0), 所以这里按 offset 截断;
        //    缓冲大小只是上界(参数最多 64 项 × 9 字节)。
        const w = new ByteWriter(bitMaps.length * 4 + 64 * 9);
        for (let i = 0; i < bitMaps.length; i++)
            w.u32(bitMaps[i]);

        const param_count = Math.min(64, bitMaps.length * 32);
        for (let i = 0; i < param_count; i++)
        {
            if (i % 32 === 31)
                continue;                      // bit31 是标志位, 不携带数据
            if (!this._hasBit(i))
                continue;                      // 未设置的参数不占字节
            const v = this._get(i);
            if (this.isReserved(i))
            {
                w.i32(v | 0);                  // 预留参数: 只有 4 字节整数
            }
            else
            {
                w.u8v(this._op(i));
                if (this.isVolt(i))          w.u32(DataTrans.toVoltageUnit(v));
                else if (this.isCurrent(i))  w.u32(DataTrans.ElectricUnit(v));
                else if (this.isTemp(i))     w.u32(DataTrans.toTempUnit(v));
                else if (this.isCapacity(i)) w.i64(DataTrans.toCapUnit(v));
                else if (this.isTime64(i))   w.u64(BigInt(Math.trunc(v)));
                else if (this.isInt32(i))    w.i32(v | 0);
                else
                    throw new Error('StepInfo.pack: 参数下标 ' + i + ' 不属于任何协议类型');
            }
        }
        const body = w.bytes().subarray(0, w.offset);

        // 3) 组包 + CRC(覆盖包头 + body)
        const head = new ByteWriter(6 + body.length + 4);
        head.u16(body.length + 10);            // 6(长度/工步行号/工步号/类型) + 4(crc)
        head.u16(this.stepLineNo);
        head.u8v(this.StepID);
        head.u8v(this.StepType);
        head.raw(body);
        const prefix = head.bytes().subarray(0, head.offset);
        const out = new ByteWriter(prefix.length + 4);
        out.raw(prefix);
        out.u32(crc32(prefix));

        const b = out.bytes().subarray(0, out.offset);
        if (buffer) buffer.push(b);
        return b;
    }

    // ---- C++ StepInfo::unpack(PackDefine.cpp:526-641) —— 与 pack 完全对称 ----
    unpack(stepPack)
    {
        const head_size = 6;    // 长度 + 工步行号 + 工步号 + 工步类型
        const crc_size = 4;
        if (!stepPack || stepPack.length < head_size + crc_size)
            return false;

        // CRC: 最后 4 字节是小端 u32, 覆盖它前面的全部字节
        const n = stepPack.length;
        const stored = (stepPack[n - 4] | (stepPack[n - 3] << 8)
            | (stepPack[n - 2] << 16) | (stepPack[n - 1] << 24)) >>> 0;
        if (stored !== crc32(stepPack.subarray(0, n - crc_size)))
            return false;

        const r = new ByteReader(stepPack);
        const packLen = r.u16();
        if (packLen !== (n & 0xffff))          // C++: packLen != (ushort)stepPack.size()
            return false;
        this.stepLineNo = r.u16();
        this.StepID = r.u8v();
        this.StepType = r.u8v();

        this.clearParams();

        // 位图: 一直读到 0(那一个也算在读到的个数里), 最多 12 个
        const bitMaps = [];
        for (let i = 0; i < 12; i++)
        {
            const m = r.u32();
            bitMaps.push(m);
            if (m === 0)
                break;
        }

        // 按位表读参数; 越界的位直接忽略(paramList 固定 64 项)
        const max = Math.min(bitMaps.length * 32, 64);
        for (let i = 0; i < max; i++)
        {
            if (i % 32 === 31)
                continue;                      // bit31 = 标志位, 不代表参数有效
            if (((bitMaps[(i / 32) | 0] >>> (i % 32)) & 1) === 0)
                continue;

            this._setBit(i);
            if (this.isReserved(i))
            {
                this.paramList[i] = r.i32();
            }
            else
            {
                this.paramOp[i] = r.u8v();
                if (this.isVolt(i))          this.paramList[i] = DataTrans.fromVoltageUnit(r.u32());
                else if (this.isCurrent(i))  this.paramList[i] = DataTrans.fromElectricUnit(r.u32());
                else if (this.isTemp(i))     this.paramList[i] = DataTrans.fromTempUnit(r.u32());
                else if (this.isCapacity(i)) this.paramList[i] = DataTrans.fromCapUnit(r.i64());
                else if (this.isTime64(i))   this.paramList[i] = Number(r.u64());
                else if (this.isInt32(i))    this.paramList[i] = r.i32();
                else
                {
                    this._clearBit(i);         // 类型不在协议表里 -> 整个包判为坏包(C++ 同款)
                    return false;
                }
            }
        }
        return true;
    }
    genDefaultStep(type)
    {
        if (type !== undefined && type !== null)
            this.StepType = type;
        return this;
    }
}

// ParamInfo / typeList 已挪进类体(见 stru_BTS_CMD_SET_STEP76_Ex_StepInfo 开头)。
// 注: 原来写成"类静态成员赋值"(类外) —— 那样 TS 不认(TS2339 "does not exist on type
//     'typeof ...'"), 而实测引擎支持 ES2022 静态字段(2026-10-04 探针确认), 所以改成类体内声明。

// C++: `struct stru_BTS_CMD_SET_STEP76_Ex : stru_BTS_CMD_STEP_REQUEST`(PackDefine.h:435)
// —— 继承是必需的: BTEServer.send(netId, pack) 靠 `instanceof stru_BTS_CMD_STEP_REQUEST`
//    分辨"包还是命令字", 工步下发走的正是这条路(否则整个下发队列都发不出去)。
export class stru_BTS_CMD_SET_STEP76_Ex extends stru_ChannelPackBase
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    static get StepInfo() { return stru_BTS_CMD_SET_STEP76_Ex_StepInfo; }

    constructor()
    {
        super();
        this.Guid = new Uint8Array(16);
        this.ChannelID = new Uint8Array(8);
        this.DataEndFlag = 0;
        this.DataIndex = 0;
        this.StepCount = 0;
        this.stepList = [];   // QVector<StepInfo>
    }
    // setChannel/clearChannel/channels 继承基类(语义与 C++ 的 setChannel(int idx) 一致)
    type() { return BTS_CMD_SET_STEP76_Ex; }                      // 0x0200

    // ---- C++ stru_BTS_CMD_SET_STEP76_Ex::pack(PackDefine.cpp:761-795) —— 工步文件包 ----
    //   Guid[16] + ChannelID[8] + StepFileLen(int32) + 工步数(ushort)
    //   + DataEndFlag(uchar) + DataIndex(ushort) + 每个工步的工步包(见 StepInfo::pack)
    // ⚠ C++ 里那两个分包字段写的是**局部常量** 0x02 / 0x01(成员值被忽略), 这里照抄。
    pack(buffer)
    {
        const packs = [];
        let fileLen = 0;
        for (let i = 0; i < this.stepList.length; i++)
        {
            const one = this.stepList[i].pack(null);   // 单个工步的包(含它自己的 crc)
            packs.push(one);
            fileLen += one.length;
        }

        const w = new ByteWriter(16 + 8 + 4 + 2 + 1 + 2 + fileLen);   // 精确大小(见上面的说明)
        w.raw(this.Guid);
        w.raw(this.ChannelID);
        w.i32(fileLen);
        w.u16(packs.length & 0xffff);
        w.u8v(0x02);        // DataEndFlag: 0x02 = 后面没有数据包了
        w.u16(0x01);        // DataIndex: 第一包
        for (let i = 0; i < packs.length; i++)
            w.raw(packs[i]);

        const b = w.bytes().subarray(0, w.offset);
        if (buffer) buffer.push(b);
        return b;
    }

    // ---- C++ stru_BTS_CMD_SET_STEP76_Ex::unpack(PackDefine.cpp:802-848) ----
    unpack(data)
    {
        const head_size = 16 + 8 + 4 + 2 + 1 + 2;
        if (!data || data.length < head_size)
            return false;

        const r = new ByteReader(data);
        this.Guid = r.bytes(16);
        this.ChannelID = r.bytes(8);
        const fileLen = r.i32();
        const stepCount = r.u16();
        this.DataEndFlag = r.u8v();
        this.DataIndex = r.u16();

        if (fileLen !== data.length - head_size)
            return false;

        this.stepList = [];
        this.StepCount = 0;
        let pos = head_size;
        for (let i = 0; i < stepCount; i++)
        {
            if (pos + 2 > data.length)
                return false;
            const stepLen = data[pos] | (data[pos + 1] << 8);
            if (stepLen < 10 || pos + stepLen > data.length)
                return false;

            const step = new stru_BTS_CMD_SET_STEP76_Ex_StepInfo();
            if (!step.unpack(data.subarray(pos, pos + stepLen)))
                return false;
            this.stepList.push(step);
            pos += stepLen;
        }
        this.StepCount = this.stepList.length;
        return true;
    }

    // 复位成"默认工步包": 清空通道位图与工步表。
    // 传了 stepType -> 生成一条该类型的默认工步(C++ 侧 genDefaultStep(stepType) 的用法);
    // 不传 -> 只得到一个空包(新建工程时每个通道一份, 等用户去编辑)。
    genDefaultStep(stepType)
    {
        this.ChannelID = new Uint8Array(8);
        this.DataEndFlag = 0;
        this.DataIndex = 0;
        this.stepList = [];
        this.StepCount = 0;

        if (stepType !== undefined && stepType !== null && stepType > 0)
        {
            const s = new stru_BTS_CMD_SET_STEP76_Ex_StepInfo();
            s.StepID = 1;
            s.genDefaultStep(stepType);
            this.stepList.push(s);
            this.StepCount = 1;
        }
        return this.updateStepLine();
    }

    // 重算所有工步的参数位图(界面改完参数后调一次) + 回填工步行号/工步号
    // (C++ PackDefine.cpp:750-759 的 updateStepLine: StepLine = StepID = 工步序号 + 1)
    updateStepLine()
    {
        for (let i = 0; i < this.stepList.length; i++)
        {
            this.stepList[i].updateStepLine();
            this.stepList[i].stepLineNo = i + 1;
            this.stepList[i].StepID = i + 1;
        }
        this.StepCount = this.stepList.length;
        return this;
    }
}

// ---------------------------------------------------------------------------
// TODO(未移植): 0x0200 SET_STEP76_Ex 工步下发
//   C++ 侧 PackDefine.cpp:435-830 的 StepInfo::pack/unpack: 两个 32 位参数位图
//   (getBitmap/setBitmap) + 按位表顺序排列的参数值(每项 1+1+N 字节, 类型由 PARAM_* 决定),
//   再叠加 QDataStream 版 operator<<(BTERecordDataBase.cpp 侧序列化)。
//   上位机**下发**工步才需要; 只做上报解析(本文件)用不到。
