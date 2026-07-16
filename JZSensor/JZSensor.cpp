#include "JZSensor.h"
#include "JZSensorDriver.h"
#include "JZDebug.h"

//JZSensor
std::vector<JZSensorInfo> JZSensor::availableSensors()
{
    return JZSensorDriver::allAvailableSensors();
}

JZSensorInfo JZSensor::findInfoById(const std::string &id)
{
    for (auto &info : JZSensorDriver::allAvailableSensors())
    {
        if (info.id == id)
            return info;
    }
    return JZSensorInfo{};   // 空对象, isNull() 为真
}

JZSensor::JZSensor(JZObject *parent)
    : JZObject(parent)
{
}

JZSensor::~JZSensor()
{
    close();
}

// ==================== 元信息 ====================
void JZSensor::setId(const std::string &id)
{
    setInfo(findInfoById(id));
}

void JZSensor::setInfo(const JZSensorInfo &info)
{
    m_info = info;
    m_drv = JZSensorDriver::findDriver(m_info.id);
    JZ_ASSERT_X(m_drv, "JZSensor: no driver found for id");
}

const JZSensorInfo &JZSensor::info() const
{
    return m_info;
}

// ==================== 打开 / 关闭 ====================
bool JZSensor::open()
{
    m_open = m_drv->open(this);
    return m_open;
}

void JZSensor::close()
{
    if (!m_open)
        return;

    m_drv->stopContinuous(this);
    m_drv->close(this);
    m_open = false;
}

bool JZSensor::isOpen() const
{
    return m_open;
}

// ==================== 周期采样 ====================
void JZSensor::start(int intervalMs)
{
    if (!m_open)
        return;

    m_drv->startContinuous(this, intervalMs > 0 ? intervalMs : 1000);
}

void JZSensor::stop()
{
    m_drv->stopContinuous(this);
}