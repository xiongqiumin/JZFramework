// ---------------------------------------------------------------------------
// storage.js —— 存档序列化(JS 原生 JSON, 取代 Qt 的 QByteArray + QDataStream)
//
// 背景(2026-09-13 用户定):
//   **存储的兼容性不用考虑, 网络的兼容性必须一致。**
//   所以:
//     - 网络报文的字节流仍然由 protocol.js / structs.js 负责, 与 C++ 版逐字节一致,
//       本文件**绝不参与**网络路径(不 import protocol/structs 的任何 pack/unpack);
//     - 存档(工程 .prj / 记录库 blob / 工步文件)改成 JSON —— JS 原生的序列化原语就是
//       JSON.stringify / JSON.parse, 不需要自造二进制流, 也不需要 QDataStream 那套
//       "operator<< 重载 + 裸 s.read() 猜类型"。
//
// 约定(各类型的 toJson/fromJson 都按这套走):
//   Date      -> ISO 字符串(null 保持 null)             原 QDateTime
//   BigInt    -> Number(协议里的 int64 都是 ms/mAh 量级, 远小于 2^53); fromJson 还原成 BigInt
//   Uint8Array-> 普通数字数组(定长位图/通道掩码等)         原 QByteArray 的定长缓冲
//   其余       -> 递归 JSON(数组/对象/字符串/数字/布尔)
//
// 版本号: 存档里带 `v`, 将来格式要变时按版本升级(不再是"用 sizeof 列表当魔数")。
// ---------------------------------------------------------------------------

export const STORAGE_VERSION = 1;

// ---- 基础类型转换 ----

/** Uint8Array -> number[] (JSON 友好) */
export function u8ToArray(u8)
{
    return u8 ? Array.from(u8) : [];
}

/** number[]|Uint8Array -> Uint8Array(定长缓冲还给原长度) */
export function arrayToU8(a, size)
{
    const out = new Uint8Array((size === undefined) ? (a ? a.length : 0) : size);
    if (a)
    {
        for (let i = 0; i < out.length && i < a.length; i++)
            out[i] = a[i] & 0xff;
    }
    return out;
}

/** Date -> ISO 字符串(null/无效 -> null) */
export function dateToJson(d)
{
    if (!(d instanceof Date) || isNaN(d.getTime()))
        return null;
    return d.toISOString();
}

/** ISO 字符串 -> Date(null/解析不出 -> null) */
export function dateFromJson(s)
{
    if (s === null || s === undefined)
        return null;
    const d = new Date(s);
    return isNaN(d.getTime()) ? null : d;
}

/** BigInt|number -> number */
export function i64ToJson(v)
{
    return (typeof v === "bigint") ? Number(v) : (v || 0);
}

/** number -> BigInt(协议层的 int64 字段统一是 BigInt) */
export function i64FromJson(v)
{
    return BigInt(v || 0);
}

// ---- 文件读写 ----
// 这里**不**用 QFile: core 的 fs 就是 JS/Node 风格的原生接口(BTEProject 早就在用它)。

export function saveJsonFile(fs, path, obj)
{
    fs.writeFileSync(path, JSON.stringify(obj, null, 2), "utf8");
}

export function loadJsonFile(fs, path)
{
    if (!fs.existsSync(path))
        return null;
    const text = fs.readFileSync(path, "utf8");
    if (!text || !String(text).trim())
        return null;
    try
    {
        return JSON.parse(String(text));
    }
    catch (e)
    {
        console.error("[storage] JSON 解析失败 " + path + ": " + e);
        return null;
    }
}

// ---- UTF-8 文本 <-> 字节(记录库 blob 用; 交付给未来的 sqlite 后端) ----
export function textToBytes(s)
{
    const c = _jzCore();
    if (c && typeof __jz !== "undefined" && __jz.bytes)
        return __jz.bytes.fromString(String(s));
    const out = [];
    const str = String(s);
    for (let i = 0; i < str.length; i++)
    {
        const code = str.charCodeAt(i);
        if (code < 0x80) out.push(code);
        else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
        else out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
    return new Uint8Array(out);
}

export function bytesToText(u8)
{
    const c = _jzCore();
    if (c && typeof __jz !== "undefined" && __jz.bytes)
        return __jz.bytes.toString(u8);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}

/** 对象 -> JSON 文本(记录库 blob 用) */
export function jsonTextOf(obj)
{
    return JSON.stringify(obj);
}

/** JSON 文本 -> 对象; 解析失败返回 null(取代原来的 recordMagic 魔数校验) */
export function parseJsonText(text)
{
    if (text === null || text === undefined || text === "")
        return null;
    try
    {
        return JSON.parse(String(text));
    }
    catch (e)
    {
        console.error("[storage] blob 不是合法 JSON, 已忽略: " + e);
        return null;
    }
}

function _jzCore()
{
    try
    {
        if (typeof __jz !== "undefined" && __jz && __jz.core)
            return __jz.core;
    }
    catch (e) { /* ignore */ }
    return null;
}

// ===========================================================================
// 各存档类型的 JSON 编解码
//
// 取代原来的 `QDataStream &operator<<(QDataStream&, const T&)` /
// `operator>>(QDataStream&, T&)` 一对函数 —— 字段集合与顺序按原实现, 只是目标载体
// 从"二进制流"换成"普通 JS 对象"。
// 命名统一 `xxxToJson(o)` / `xxxSetJson(o, j)`: 前者**返回新对象**, 后者**就地填 o**
// (与原来 readXxx(s, param) 的出参风格一致)。
//
// 注意: 这里的字段名一律用**域对象的字段名**, 不涉及网络报文的字节布局 ——
// 存档格式与 0x0200 等报文的 pack/unpack 完全无关(网络兼容性由 structs.js 保证)。
// ===========================================================================

// ---- stru_BTS_CMD_SEND_INFO (设备信息) ----
export function sendInfoToJson(o)
{
    return {
        Version: o.Version,
        Fixed: u8ToArray(o.Fixed),
        Type: o.Type, DataEndFlag: o.DataEndFlag, DataIndex: o.DataIndex,
        DeviceType: o.DeviceType, TypeNum: o.TypeNum,
        Channel: u8ToArray(o.Channel),
        ChannelType: o.ChannelType,
        ChargeMax: o.ChargeMax, DischargeMax: o.DischargeMax, VoltageMax: o.VoltageMax,
        TempatureMax: o.TempatureMax, InternalResistanceMax: o.InternalResistanceMax,
        CurrentNum: o.CurrentNum, CurrentRange: (o.CurrentRange || []).slice(),
        ChannelNum: o.ChannelNum, DeviceString: o.DeviceString,
    };
}
export function sendInfoSetJson(o, j)
{
    if (!j) return o;
    o.Version = j.Version || 0;
    o.Fixed = arrayToU8(j.Fixed, 6);
    o.Type = j.Type || 0; o.DataEndFlag = j.DataEndFlag || 0; o.DataIndex = j.DataIndex || 0;
    o.DeviceType = j.DeviceType || 0; o.TypeNum = j.TypeNum || 0;
    o.Channel = arrayToU8(j.Channel, 32);
    o.ChannelType = j.ChannelType || 0;
    o.ChargeMax = j.ChargeMax || 0; o.DischargeMax = j.DischargeMax || 0;
    o.VoltageMax = j.VoltageMax || 0; o.TempatureMax = j.TempatureMax || 0;
    o.InternalResistanceMax = j.InternalResistanceMax || 0;
    o.CurrentNum = j.CurrentNum || 0;
    o.CurrentRange = (j.CurrentRange || []).slice();
    while (o.CurrentRange.length < 10) o.CurrentRange.push(0);
    o.ChannelNum = j.ChannelNum || 0; o.DeviceString = j.DeviceString || "";
    return o;
}

// ---- stru_BTS_CMD_SEND_RTDATA76 (实时帧; 也是 BTERecordInfo.rtData) ----
export function rtDataToJson(o)
{
    return {
        DFDataPointer: o.DFDataPointer, RTDataCount: o.RTDataCount, DataFlag: o.DataFlag,
        ChannelIDByNum: o.ChannelIDByNum, AuxChlIDByNum: o.AuxChlIDByNum,
        CycleStatus: o.CycleStatus, StepID: o.StepID, StepType: o.StepType, WorkType: o.WorkType,
        TestTime: i64ToJson(o.TestTime),
        Voltage: o.Voltage, Current: o.Current,
        InternalResistance: o.InternalResistance, Tempature: o.Tempature,
        ChargeCap: i64ToJson(o.ChargeCap), DisChargeCap: i64ToJson(o.DisChargeCap),
        ChargeEng: i64ToJson(o.ChargeEng), DisChargeEng: i64ToJson(o.DisChargeEng),
        CurStepRange: o.CurStepRange, PrtCode: o.PrtCode,
    };
}
export function rtDataSetJson(o, j)
{
    if (!j) return o;
    o.DFDataPointer = j.DFDataPointer || 0; o.RTDataCount = j.RTDataCount || 0;
    o.DataFlag = j.DataFlag || 0;
    o.ChannelIDByNum = j.ChannelIDByNum || 0; o.AuxChlIDByNum = j.AuxChlIDByNum || 0;
    o.CycleStatus = j.CycleStatus || 0; o.StepID = j.StepID || 0;
    o.StepType = j.StepType || 0; o.WorkType = j.WorkType || 0;
    o.TestTime = i64FromJson(j.TestTime);
    o.Voltage = j.Voltage || 0; o.Current = j.Current || 0;
    o.InternalResistance = j.InternalResistance || 0; o.Tempature = j.Tempature || 0;
    o.ChargeCap = i64FromJson(j.ChargeCap); o.DisChargeCap = i64FromJson(j.DisChargeCap);
    o.ChargeEng = i64FromJson(j.ChargeEng); o.DisChargeEng = i64FromJson(j.DisChargeEng);
    o.CurStepRange = j.CurStepRange || 0; o.PrtCode = j.PrtCode || 0;
    return o;
}

// ---- stru_BTS_CMD_SET_STEP76_Ex (工步包) + 内嵌 StepInfo ----
//   注意: 这里只序列化"工步数据本身"; 它在**网络**上的 0x0200 字节布局仍由(尚未收口的)
//   StepInfo::pack 负责, 两者互不影响。
export function stepPackToJson(o)
{
    const list = [];
    for (let i = 0; i < (o.stepList || []).length; i++)
        list.push(stepPackStepToJson(o.stepList[i]));
    return {
        Guid: u8ToArray(o.Guid), ChannelID: u8ToArray(o.ChannelID),
        DataEndFlag: o.DataEndFlag, DataIndex: o.DataIndex,
        StepCount: o.StepCount, stepList: list,
    };
}
// makeStepInfo: 建 StepInfo 的工厂(由调用方传入, storage.js 不依赖报文层的类)
export function stepPackSetJson(o, j, makeStepInfo)
{
    if (!j) return o;
    o.Guid = arrayToU8(j.Guid, 16);
    o.ChannelID = arrayToU8(j.ChannelID, 8);
    o.DataEndFlag = j.DataEndFlag || 0; o.DataIndex = j.DataIndex || 0;
    o.StepCount = j.StepCount || 0;
    o.stepList = [];
    const list = j.stepList || [];
    for (let i = 0; i < list.length; i++)
    {
        const s = makeStepInfo ? makeStepInfo() : null;
        if (!s)
            break;
        stepPackStepSetJson(s, list[i]);
        o.stepList.push(s);
    }
    return o;
}

// stru_BTS_CMD_SET_STEP76_Ex::StepInfo(工步包里的单个工步)
function stepPackStepToJson(s)
{
    return {
        StepLine: u8ToArray(s.StepLine),
        paramList: (s.paramList || []).map((v) => (typeof v === "bigint") ? Number(v) : v),
        StepType: s.StepType, StepID: s.StepID,
    };
}
function stepPackStepSetJson(s, j)
{
    if (!j) return s;
    s.StepLine = arrayToU8(j.StepLine, 8);
    s.paramList = (j.paramList || []).slice();
    s.StepType = j.StepType || 0; s.StepID = j.StepID || 0;
    return s;
}

// ---- BTEStepInfo(记录层的"工步信息": 起始工步 + 电池信息 + 下发工步) ----
export function btStepInfoToJson(o)
{
    return {
        startStepIndex: o.startStepIndex,
        creator: o.creator, cellBatch: o.cellBatch, cellNo: o.cellNo, memo: o.memo,
        step: o.step ? stepPackToJson(o.step) : null,
    };
}
export function btStepInfoSetJson(o, j)
{
    if (!j) return o;
    o.startStepIndex = j.startStepIndex || 0;
    o.creator = j.creator || "";
    o.cellBatch = j.cellBatch || "";
    o.cellNo = j.cellNo || "";
    o.memo = j.memo || "";
    if (j.step && o.step)
        stepPackSetJson(o.step, j.step, o.makeStepInfo || null);
    return o;
}

// ---- DeviceData(设备: 设备信息 + 64 通道工步 + 64 通道记录信息 + 并联信息) ----
//   makeBTEStepInfo: 建 BTEStepInfo 的工厂 —— DeviceData.stepList 在 JS 侧是**空槽**
//   (C++ 是定长数组, 元素默认构造), 所以读档时得由调用方提供工厂。
export function deviceDataToJson(o)
{
    const steps = [];
    for (let i = 0; i < o.stepList.length; i++)
        steps.push(o.stepList[i] ? btStepInfoToJson(o.stepList[i]) : null);

    const infos = [];
    for (let i = 0; i < o.recordList.length; i++)
        infos.push(recordInfoToJson(o.recordList[i].info));

    const plist = [];
    for (let i = 0; i < (o.parallelList || []).length; i++)
        plist.push(parallelInfoToJson(o.parallelList[i]));

    return {
        deviceId: o.deviceId,
        deviceInfo: sendInfoToJson(o.deviceInfo),
        stepList: steps,
        recordInfoList: infos,      // 原 writeDeviceData 写的是 recordList[i].info
        parallelList: plist,
        enableItem: o.enableItem,
        itemSize: o.itemSize,
    };
}
export function deviceDataSetJson(o, j, makeBTEStepInfo)
{
    if (!j) return o;
    o.deviceId = j.deviceId || "";

    if (o.deviceInfo)
        sendInfoSetJson(o.deviceInfo, j.deviceInfo);

    const steps = j.stepList || [];
    for (let i = 0; i < o.stepList.length; i++)
    {
        if (!steps[i])
            continue;
        if (!o.stepList[i] && makeBTEStepInfo)
            o.stepList[i] = makeBTEStepInfo();
        if (o.stepList[i])
            btStepInfoSetJson(o.stepList[i], steps[i]);
    }

    const infos = j.recordInfoList || [];
    for (let i = 0; i < o.recordList.length; i++)
        if (infos[i])
            recordInfoSetJson(o.recordList[i].info, infos[i]);

    // ParallelInfo 只有 channels 一个字段, 且业务侧按"有 .channels"使用 -> 普通对象即可
    o.parallelList = [];
    const plist = j.parallelList || [];
    for (let i = 0; i < plist.length; i++)
        o.parallelList.push({ channels: (plist[i].channels || []).slice() });

    o.enableItem = (j.enableItem === undefined) ? o.enableItem : j.enableItem;
    o.itemSize = (j.itemSize === undefined) ? o.itemSize : j.itemSize;
    return o;
}

// ---- BTERecordInfo(+DataCache/rtData/stepInfo) ----
//   字段集合与原来的 writeBTERecordInfo 一致(DeviceID 不在其中: 记录库是按设备分库的)
export function recordInfoToJson(o)
{
    const dc = o.dataCache || {};
    return {
        itemIndex: o.itemIndex, recordId: o.recordId,
        recordTime: dateToJson(o.recordTime), recordTimeEnd: dateToJson(o.recordTimeEnd),
        recordNum: o.recordNum,
        stepInfo: btStepInfoToJson(o.stepInfo),
        rtData: o.rtData ? rtDataToJson(o.rtData) : null,
        dataCache: {
            beginDis: dc.beginDis, endDis: dc.endDis, midDisVoltage: dc.midDisVoltage,
            endCap: dc.endCap, endDisCap: dc.endDisCap, endDisEng: dc.endDisEng,
            endDisTestTime: dc.endDisTestTime,
            beginChg: dc.beginChg, endChg: dc.endChg, ccRatio: dc.ccRatio,
        },
    };
}
export function recordInfoSetJson(o, j)
{
    if (!j) return o;
    o.itemIndex = (j.itemIndex === undefined) ? -1 : j.itemIndex;
    o.recordId = (j.recordId === undefined) ? -1 : j.recordId;
    o.recordTime = dateFromJson(j.recordTime);
    o.recordTimeEnd = dateFromJson(j.recordTimeEnd);
    o.recordNum = j.recordNum || 0;
    // 嵌套对象由各自类的构造函数建好(BTERecordInfo ctor 里就有 stepInfo/rtData/dataCache);
    // 工步包里的 stepList 需要造 StepInfo -> 由 BTEStepInfo 提供 makeStepInfo 工厂
    if (o.stepInfo)
        btStepInfoSetJson(o.stepInfo, j.stepInfo);
    if (j.rtData && o.rtData)
        rtDataSetJson(o.rtData, j.rtData);
    const dc = o.dataCache, jc = j.dataCache || {};
    if (dc)
    {
        dc.beginDis = num(jc.beginDis, -1); dc.endDis = num(jc.endDis, -1);
        dc.midDisVoltage = num(jc.midDisVoltage, 0);
        dc.endCap = num(jc.endCap, 0); dc.endDisCap = num(jc.endDisCap, 0);
        dc.endDisEng = num(jc.endDisEng, 0); dc.endDisTestTime = num(jc.endDisTestTime, 0);
        dc.beginChg = num(jc.beginChg, -1); dc.endChg = num(jc.endChg, -1);
        dc.ccRatio = num(jc.ccRatio, 0);
    }
    return o;
}
function num(v, def) { return (v === undefined || v === null) ? def : v; }

// ---- stru_BTS_CMD_SEND_DFDATA76::Data(记录点) ----
export function dfDataToJson(d)
{
    return {
        DataFlag: d.DataFlag, SubDevID: d.SubDevID, ChannelIDByNum: d.ChannelIDByNum,
        AuxChlIDByNum: d.AuxChlIDByNum, TestID: d.TestID, DataSerial: d.DataSerial,
        CycleStatus: d.CycleStatus, StepID: d.StepID, StepType: d.StepType,
        StepChangeCount: d.StepChangeCount, WorkType: d.WorkType,
        TestTime: i64ToJson(d.TestTime),
        Voltage: d.Voltage, Current: d.Current,
        InternalResistance: d.InternalResistance, Tempature: d.Tempature,
        ChargeCap: i64ToJson(d.ChargeCap), DisChargeCap: i64ToJson(d.DisChargeCap),
        ChargeEng: i64ToJson(d.ChargeEng), DisChargeEng: i64ToJson(d.DisChargeEng),
        ATime: d.ATime ? {
            year: d.ATime.year, month: d.ATime.month, day: d.ATime.day,
            hour: d.ATime.hour, minute: d.ATime.minute, second: d.ATime.second,
        } : null,
        CurStepRange: d.CurStepRange,
    };
}
export function dfDataSetJson(d, j)
{
    if (!j) return d;
    d.DataFlag = j.DataFlag || 0; d.SubDevID = j.SubDevID || 0;
    d.ChannelIDByNum = j.ChannelIDByNum || 0; d.AuxChlIDByNum = j.AuxChlIDByNum || 0;
    d.TestID = j.TestID || 0; d.DataSerial = j.DataSerial || 0;
    d.CycleStatus = j.CycleStatus || 0; d.StepID = j.StepID || 0;
    d.StepType = j.StepType || 0; d.StepChangeCount = j.StepChangeCount || 0;
    d.WorkType = j.WorkType || 0;
    d.TestTime = i64FromJson(j.TestTime);
    d.Voltage = j.Voltage || 0; d.Current = j.Current || 0;
    d.InternalResistance = j.InternalResistance || 0; d.Tempature = j.Tempature || 0;
    d.ChargeCap = i64FromJson(j.ChargeCap); d.DisChargeCap = i64FromJson(j.DisChargeCap);
    d.ChargeEng = i64FromJson(j.ChargeEng); d.DisChargeEng = i64FromJson(j.DisChargeEng);
    d.ATime = j.ATime ? {
        year: j.ATime.year, month: j.ATime.month, day: j.ATime.day,
        hour: j.ATime.hour, minute: j.ATime.minute, second: j.ATime.second,
    } : { year: 0, month: 0, day: 0, hour: 0, minute: 0, second: 0 };
    d.CurStepRange = j.CurStepRange || 0;
    return d;
}

// ---- SplitInfo(+Range) / ParallelInfo ----
export function splitInfoToJson(o)
{
    const rows = [];
    for (let i = 0; i < (o.rangeList || []).length; i++)
    {
        const row = [];
        for (let j = 0; j < o.rangeList[i].length; j++)
        {
            const r = o.rangeList[i][j];
            row.push({ vaild: !!r.vaild, upper: r.upper, lower: r.lower });
        }
        rows.push(row);
    }
    return { name: o.name, rangeList: rows };
}
export function splitInfoSetJson(o, j)
{
    if (!j) return o;
    o.name = j.name || "";
    const rows = j.rangeList || [];
    for (let i = 0; i < 2 && i < o.rangeList.length; i++)
    {
        for (let k = 0; k < o.rangeList[i].length; k++)
        {
            const jr = (rows[i] && rows[i][k]) ? rows[i][k] : null;
            const r = o.rangeList[i][k];
            r.vaild = jr ? !!jr.vaild : false;
            r.upper = jr ? (jr.upper || 0) : 0;
            r.lower = jr ? (jr.lower || 0) : 0;
        }
    }
    return o;
}
export function parallelInfoToJson(o)
{
    return { channels: (o.channels || []).slice() };
}
export function parallelInfoSetJson(o, j)
{
    o.channels = (j && j.channels) ? j.channels.slice() : [];
    return o;
}

