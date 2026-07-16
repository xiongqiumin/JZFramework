#ifndef JZ_TEMPERATURE_SENSOR_H_
#define JZ_TEMPERATURE_SENSOR_H_

#include "JZSensor.h"
#include <cmath>
#include <vector>

// 温度传感器的具体子类。实现 onSampled, 缓存最近一次温度(已校准)与原始分量。
// 用法:
//   JZTemperatureSensor t;
//   t.setInfo(JZSensor::findInfoById("esp32_die_temp"));
//   t.open();
//   t.start(1000);                 // 周期采样, 经 onSampled 刷新 temperature()
//   double v = t.temperature();    // 最近一次温度(℃)
class JZTemperatureSensor : public JZSensor
{
public:
    explicit JZTemperatureSensor(JZObject *parent = nullptr);

    // 最近一次温度(已校准), 单位见 info().unit。失败/未采样返回 NaN。
    double temperature() const;

    // 最近一次原始分量(多轴传感器用), 至少含 1 个元素。
    const std::vector<double> &raw() const;

protected:
    void onSampled(double *raw, int size) override;

    double              m_lastValue = std::nan("");
    std::vector<double> m_raw;
};

#endif // !JZ_TEMPERATURE_SENSOR_H_
