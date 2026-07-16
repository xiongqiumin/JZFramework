#ifndef JZ_SENSOR_H_
#define JZ_SENSOR_H_

#include "JZObject.h"
#include "JZSensorInfo.h"

class JZSensorDriver;

// JZSensor 是面向用户的传感器类。自身不做平台相关操作,
// 所有底层采样委派给 JZSensorDriver (如后续实现的 JZSensorDriverEsp32)。
//
// 功能:
//   - 周期采样              -> start(ms) 启动, 驱动按间隔回调 onSampled()
//   - 线性校准(offset/gain) -> 对采样值做补偿(由子类在 onSampled 中应用)
//
// 传感器元信息(类型/通道/单位/id)由 JZSensorInfo 承载, 直接保存在实例中。
class JZSensor : public JZObject
{
public:
    // 枚举当前平台可用传感器(委派给驱动)
    static std::vector<JZSensorInfo> availableSensors();

    // 按 id 查找可用传感器信息(找不到返回 isNull() 为真的空对象)
    static JZSensorInfo findInfoById(const std::string &id);

    explicit JZSensor(JZObject *parent = nullptr);
    virtual ~JZSensor();

    // 传感器元信息(类型/通道/单位/id 等), 直接保存 JZSensorInfo
    void setId(const std::string &id);
    void setInfo(const JZSensorInfo &info);
    const JZSensorInfo &info() const;

    // 打开(绑定驱动并应用配置) / 关闭(释放)
    // 打开前需先 setInfo() 设置好元信息(类型/通道/单位/id)。
    bool open();
    void close();
    bool isOpen() const;

    // ============ 周期采样 ============
    // start(): 启动周期采样, 驱动按 intervalMs 间隔回调 onSampled()
    // stop() : 停止周期采样
    void start(int intervalMs);
    void stop();

    // 由底层驱动异步调用: 上报一次采样值(可多轴, raw[0..size-1])。
    // 子类必须实现: 在此做校准/缓存/分发等处理。
    virtual void onSampled(double *raw, int size) = 0;

protected:
    JZ_DISABLE_COPY(JZSensor)

    JZSensorDriver *m_drv = nullptr;
    JZSensorInfo    m_info;             // 传感器元信息(类型/通道/单位/id)
    bool            m_open = false;
};

#endif // !JZ_SENSOR_H_
