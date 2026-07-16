#include "JZTemperatureSensor.h"

JZTemperatureSensor::JZTemperatureSensor(JZObject *parent)
    : JZSensor(parent)
{
}

double JZTemperatureSensor::temperature() const
{
    return m_lastValue;
}

const std::vector<double> &JZTemperatureSensor::raw() const
{
    return m_raw;
}

void JZTemperatureSensor::onSampled(double *raw, int size)
{
    if (!raw || size <= 0)
        return;

    m_raw.assign(raw, raw + size);
    // 主通道(轴0)作为温度值, 应用校准系数
    m_lastValue = raw[0];
}
