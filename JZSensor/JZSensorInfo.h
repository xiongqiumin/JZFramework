#ifndef JZ_SENSOR_INFO_H_
#define JZ_SENSOR_INFO_H_
#include "JZGlobal.h"
#include <string>
#include <vector>

// 传感器测量类型
enum class JZFRAMEWORK_API JZSensorType {
    Custom = 0,
    Temperature,   // 温度
    Humidity,      // 湿度
    Pressure,      // 气压
    Light,         // 光照
    Accel,         // 加速度
    Gyro,          // 角速度
    Altitude,      // 海拔
};

// 测量单位
enum class JZFRAMEWORK_API JZSensorUnit {
    None = 0,
    Celsius,       // ℃
    Fahrenheit,    // ℉
    PercentRH,     // %RH
    HPa,           // 百帕
    Lux,           // 照度
    G,             // 重力加速度
    DPS,           // 度/秒
    Meter,         // 米
    Raw,           // 原始值
};

// 描述一个可用传感器通道的静态信息（枚举结果）。
// 各平台驱动在 JZSensorDriver::availableSensors() 中填充并上报。
class JZFRAMEWORK_API JZSensorInfo
{
public:
    // 能力(位掩码, 可组合)
    enum Capability {
        CapNone       = 0,
        CapOnDemand   = 0x1,   // 支持 read() 即时采样
        CapContinuous = 0x2,   // 支持周期采样(推送 sigValue)
    };

    // 是否为空（无有效标识）
    bool isNull() const { return name.empty(); }

    // 能力查询/设置
    void setCapability(Capability cap) { capability = cap; }
    bool canOnDemand()   const { return (capability & CapOnDemand)   != 0; }
    bool canContinuous() const { return (capability & CapContinuous) != 0; }

    JZSensorType  type = JZSensorType::Custom;  // 测量类型
    JZSensorUnit  unit = JZSensorUnit::None;    // 单位
    int           channel = 0;                  // 同设备多通道序号(寻址键, IMU 的 x/y/z)
    std::string   id;                            // 全局唯一稳定句柄(枚举时分配, 供 UI/云端引用)
    std::string   name;                         // 标识名, 如 "ESP32 Internal Temp"
    std::string   description;                  // 描述
    Capability    capability = CapOnDemand;     // 能力
};

#endif // !JZ_SENSOR_INFO_H_
