#ifndef JZ_SENSOR_DRIVER_H_
#define JZ_SENSOR_DRIVER_H_

#include <vector>
#include "JZObject.h"
#include "JZSensorInfo.h"

class JZSensor;

// 传感器驱动抽象基类。一个驱动代表一个物理传感器设备(如 ESP32 内部温度传感器、
// BME280), 可同时提供多种测量类型(温度/湿度/气压)。
// 通过 addInstance() 注册, JZSensor 按 id 查找对应驱动。
class JZSensorDriver
{
public:
    JZSensorDriver() = default;
    virtual ~JZSensorDriver() = default;

    static void addInstance(JZSensorDriver *inst);
    static void clearInstances();
    static std::vector<JZSensorDriver*> instances();

    // 根据传感器 id 查找驱动, 找不到返回 nullptr
    static JZSensorDriver *findDriver(const std::string &id);

    // 汇总所有驱动的可用传感器列表
    static std::vector<JZSensorInfo> allAvailableSensors();

    // 查询本驱动是否提供指定 id 的传感器
    virtual bool supportsId(const std::string &id) = 0;

    // 枚举本驱动可用传感器通道
    virtual std::vector<JZSensorInfo> availableSensors() = 0;

    // 打开 / 关闭 / 查询
    virtual bool open(JZSensor *sensor) = 0;
    virtual void close(JZSensor *sensor) = 0;
    virtual bool isOpen(JZSensor *sensor) = 0;

    // 即时采样, 返回测量值(失败返回 NaN)
    virtual double read(JZSensor *sensor) = 0;

    // 周期采样: 驱动按 intervalMs 推送采样值到 sensor->onSampled()。
    // 不需要周期采样的驱动可返回 false。
    virtual bool startContinuous(JZSensor *sensor, int intervalMs) = 0;
    virtual void stopContinuous(JZSensor *sensor) = 0;

protected:
    JZ_DISABLE_COPY(JZSensorDriver)

    static std::vector<JZSensorDriver*> s_instances;
};

#endif // !JZ_SENSOR_DRIVER_H_
