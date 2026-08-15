#ifndef JZ_SENSOR_DRIVER_SIMULATOR_H_
#define JZ_SENSOR_DRIVER_SIMULATOR_H_

#include "JZSensorDriver.h"
#include <string>
#include <vector>

// 传感器模拟驱动(暂未实现)。仅作为 JZSimulator 平台下的占位实现,
// 保证 JZDriver::init 在 simulator 配置下可正常注册。功能待补充。
class JZSensorDriverSimulator : public JZSensorDriver
{
public:
    JZSensorDriverSimulator();
    ~JZSensorDriverSimulator() override;

    // 取当前 Sensor 模拟驱动的(首个)实例(多实例模型, 仅作路由目标)。
    static JZSensorDriverSimulator *simInstance();

    // 由 JZSimServer 按模块段路由调用, 处理下行(Sensor 段)网络包
    void onRecv(JZSimPack *pack);

    bool supportsId(const std::string &id) override;

private:
    static JZSensorDriverSimulator *s_inst; // 首个实例(路由目标)
    std::vector<JZSensorInfo> availableSensors() override;

    bool open(JZSensor *sensor) override;
    void close(JZSensor *sensor) override;
    bool isOpen(JZSensor *sensor) override;

    double read(JZSensor *sensor) override;

    bool startContinuous(JZSensor *sensor, int intervalMs) override;
    void stopContinuous(JZSensor *sensor) override;
};

#endif // !JZ_SENSOR_DRIVER_SIMULATOR_H_
