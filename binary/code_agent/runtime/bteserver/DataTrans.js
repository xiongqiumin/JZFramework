// ---------------------------------------------------------------------------
// DataTrans.js —— 对应 C++ common/DataTrans.h + DataTrans.cpp
//
// 单位换算。原注释(DataTrans.h 的整段说明)见下, 一字未改:
//
//   (1).电流、容量与量程关系：
//       容量，需根据电流量程除以相应值(scale * 3600)
//           if (电流量程 < 10mA)      scale = 10000.0;
//           else if (电流量程 < 100 mA)  scale = 1000.0;
//           else if (电流量程 < 1000 mA) scale = 100.0;
//           else                        scale = 10.0;
//       实际电流 (mA) = cur / scale;
//       真实容量（mAh） = cap / 3600 / scale;
//       电压都是以0.1mV为单位，比如10000就表示为1V
//       功率：需根据电流量程计算
//       内阻是以0.1mohm为单位，比如10000就表示为1ohm
//       温度：实际温度 = temp / 10.0。//比例系数扩大10倍。然后再缩小10倍。
//       功率：实际功率 = power / scale 。经过变换，最终比例系数扩大scale倍，然后缩小scale倍。
//       内阻：实际阻值 = RI / 10000.0*(1000.0 / (10.0 / nScale)。
//           = RI*nScale / 100.0
//       能量：实际能量（mWh）： = Eng / 3600 / scale;
//
//   (2).关于保护参数设置负值：
//       下限电流，下限电压，下限温度，如果设置为负值时，在传输时，先将负数乘以比例系数，转换成补码，
//       以字节流的形式传输。在解析这些字节流的时候，先取得补码，转换成负数后，然后再除以比例系数。
//       传输值：cur, cap, temp, power, RI, Eng;
//   (3).百分比参数：
//       缩放比例系数，100倍。（不包含%）。Eg：0.2% 那么就扩大0。2×100 = 20.
//
// Qt -> JS: **不用时间类型** —— QTime 在 C++ 里只是"从 0 点起的毫秒"容器, JS 侧直接按
//           毫秒换算成字符串(见 fromTimeUnit), 反向解析也不再需要 QTime::fromString;
//           QString::asprintf("%02d") 用 padStart(2,'0');
//           QString::mid/split/toInt 用 String 的对应方法。
// ---------------------------------------------------------------------------

import { MS_PER_DAY } from './PackDefine.js';

export const DataTrans = {
    // 电流
    // uint32_t ElectricUnit(double var);
    ElectricUnit: function (var_) { return (toNum(var_) * 10000.0) >>> 0; },
    // double fromElectricUnit(int var);
    fromElectricUnit: function (var_) { return toNum(var_) / 10000.0; },

    // 温度
    // uint32_t toTempUnit(double var);
    toTempUnit: function (var_) { return (toNum(var_) * 10) >>> 0; },
    // double fromTempUnit(int var);
    fromTempUnit: function (var_)
    {
        // 注意:必须是10.0,整数除法会丢掉0.1℃精度(如55.5会变成55.0)
        return toNum(var_) / 10.0;
    },

    // 电压 mV
    // uint32_t toVoltageUnit(double var);
    toVoltageUnit: function (var_) { return (toNum(var_) * 10000) >>> 0; },
    // double fromVoltageUnit(int var);
    fromVoltageUnit: function (var_) { return toNum(var_) / 10000.0; },

    // 时间
    //   C++ 用 QTime 当时长容器(从 0 点起加毫秒); JS 侧直接按毫秒换算, 不需要时间类型。
    //   注意: 上报帧里的时间字段是 **int64 -> BigInt**(如 RTDATA76.TestTime), 所以入口先 toNum。
    // QString fromTimeUnit(uint32_t var);   -> "总小时:分:秒:毫秒" (小时是按天累加的)
    fromTimeUnit: function (var_)
    {
        const v = toNum(var_);
        const day = Math.floor(v / MS_PER_DAY);
        const t = v % MS_PER_DAY;                       // 当天内的毫秒
        const hh = day * 24 + Math.floor(t / 3600000);
        return pad2(hh) + ":" + pad2(Math.floor(t / 60000) % 60) + ":"
            + pad2(Math.floor(t / 1000) % 60) + ":" + pad3(t % 1000);   // 原 QTime::toString("mm:ss:zzz")
    },

    // QString fromTimeUnitSec(uint32_t var);
    fromTimeUnitSec: function (var_)
    {
        const v = toNum(var_);
        const day = Math.floor(v / MS_PER_DAY);
        const t = v % MS_PER_DAY;
        const hh = day * 24 + Math.floor(t / 3600000);
        return pad2(hh) + ":" + pad2(Math.floor(t / 60000) % 60) + ":"
            + pad2(Math.floor(t / 1000) % 60);                          // 原 QTime::toString("mm:ss")
    },

    // 能量
    // int64_t toEngUnit(double var);
    toEngUnit: function (var_) { return BigInt(Math.trunc(toNum(var_) * 3600.0 * 10000.0)); },
    // double fromEngUnit(int64_t var);
    fromEngUnit: function (var_) { return toNum(var_) / 3600.0 / 10000.0; },

    // 容量
    // int64_t toCapUnit(double var);
    toCapUnit: function (var_) { return BigInt(Math.trunc(toNum(var_) * 3600.0 * 10000.0)); },
    // double fromCapUnit(int64_t var);
    fromCapUnit: function (var_) { return toNum(var_) / 3600.0 / 10000.0; },

    // uint32_t toTimeUnit(QString text);   text = fromTimeUnit 的产物 "小时:分:秒:毫秒"
    toTimeUnit: function (text)
    {
        const list = String(text).split(":");
        console.assert(list.length === 4);

        // 原: QTime::fromString(text.substr(3), "mm:ss:zzz") + msecsSinceStartOfDay()
        const hh = parseInt(list[0], 10) || 0;
        const mm = parseInt(list[1], 10) || 0;
        const ss = parseInt(list[2], 10) || 0;
        const zzz = parseInt(list[3], 10) || 0;
        return (hh * 3600000 + mm * 60000 + ss * 1000 + zzz) >>> 0;
    },
};

function pad2(v)
{
    return String(v).padStart(2, '0');
}

function pad3(v)
{
    return String(v).padStart(3, '0');
}

// 协议层的 int64 字段在 JS 侧是 BigInt, 而这里全是浮点换算 —— 统一先转 Number。
// (值域是 ms / mAh / mWh 量级, 远小于 2^53, 转换无损)
function toNum(v)
{
    return (typeof v === 'bigint') ? Number(v) : (v || 0);
}
