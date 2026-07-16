#include "JZSensorDriverEsp32.h"

#include <cmath>

#include "esp_timer.h"
#include "driver/temperature_sensor.h"

const char *JZSensorDriverEsp32::kId = "esp32_die_temp";

JZSensorDriverEsp32::JZSensorDriverEsp32() = default;

JZSensorDriverEsp32::~JZSensorDriverEsp32()
{
    stopContinuous(nullptr);
    if (m_open)
    {
        temperature_sensor_disable(m_handle);
        temperature_sensor_uninstall(m_handle);
        m_handle = nullptr;
        m_open = false;
    }
}

bool JZSensorDriverEsp32::supportsId(const std::string &id)
{
    return id == kId;
}

std::vector<JZSensorInfo> JZSensorDriverEsp32::availableSensors()
{
    JZSensorInfo info;
    info.type       = JZSensorType::Temperature;
    info.unit       = JZSensorUnit::Celsius;
    info.channel    = 0;
    info.id         = kId;
    info.name       = "ESP32 Internal Temp";
    info.description = "ESP32 芯片结温(非环境温度)";
    info.capability = static_cast<JZSensorInfo::Capability>(
        JZSensorInfo::CapOnDemand | JZSensorInfo::CapContinuous);
    return { info };
}

bool JZSensorDriverEsp32::open(JZSensor *sensor)
{
    (void)sensor;
    if (m_open)
        return true;

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &m_handle) != ESP_OK)
        return false;
    if (temperature_sensor_enable(m_handle) != ESP_OK)
    {
        temperature_sensor_uninstall(m_handle);
        m_handle = nullptr;
        return false;
    }
    m_open = true;
    return true;
}

void JZSensorDriverEsp32::close(JZSensor *sensor)
{
    stopContinuous(sensor);
    if (m_open)
    {
        temperature_sensor_disable(m_handle);
        temperature_sensor_uninstall(m_handle);
        m_handle = nullptr;
        m_open = false;
    }
}

bool JZSensorDriverEsp32::isOpen(JZSensor *sensor)
{
    (void)sensor;
    return m_open;
}

double JZSensorDriverEsp32::read(JZSensor *sensor)
{
    (void)sensor;
    if (!m_open)
        return std::nan("");

    float c = 0.0f;
    if (temperature_sensor_get_celsius(m_handle, &c) != ESP_OK)
        return std::nan("");
    return static_cast<double>(c);
}

void JZSensorDriverEsp32::timerCallback(void *arg)
{
    auto *self = static_cast<JZSensorDriverEsp32 *>(arg);
    if (!self || !self->m_open || !self->m_contSensor)
        return;

    float c = 0.0f;
    if (temperature_sensor_get_celsius(self->m_handle, &c) == ESP_OK)
    {
        double v = static_cast<double>(c);
        self->m_contSensor->onSampled(&v, 1);
    }
}

bool JZSensorDriverEsp32::startContinuous(JZSensor *sensor, int intervalMs)
{
    if (!m_open || !sensor)
        return false;

    stopContinuous(sensor);
    m_contSensor = sensor;

    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = &JZSensorDriverEsp32::timerCallback;
    args.arg      = this;
    args.name     = "jz_temp";
    if (esp_timer_create(&args, &timer) != ESP_OK)
        return false;

    int64_t period_us = (intervalMs > 0 ? (int64_t)intervalMs : 1000) * 1000;
    if (esp_timer_start_periodic(timer, period_us) != ESP_OK)
    {
        esp_timer_delete(timer);
        m_contSensor = nullptr;
        return false;
    }

    m_timer = timer;
    return true;
}

void JZSensorDriverEsp32::stopContinuous(JZSensor *sensor)
{
    if (m_timer)
    {
        esp_timer_stop(static_cast<esp_timer_handle_t>(m_timer));
        esp_timer_delete(static_cast<esp_timer_handle_t>(m_timer));
        m_timer = nullptr;
    }
    m_contSensor = nullptr;
}
