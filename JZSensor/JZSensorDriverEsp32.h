#ifndef JZ_SENSOR_DRIVER_ESP32_H_
#define JZ_SENSOR_DRIVER_ESP32_H_

#include "JZSensorDriver.h"

// ESP32 内部温度传感器驱动(芯片 die 温度, 非环境温度)。
// 无需任何外部引脚, 直接读取片上温度传感器。
class JZSensorDriverEsp32 : public JZSensorDriver
{
public:
    JZSensorDriverEsp32();
    virtual ~JZSensorDriverEsp32();

    bool supportsId(const std::string &id) override;
    std::vector<JZSensorInfo> availableSensors() override;

    bool open(JZSensor *sensor) override;
    void close(JZSensor *sensor) override;
    bool isOpen(JZSensor *sensor) override;

    double read(JZSensor *sensor) override;

    bool startContinuous(JZSensor *sensor, int intervalMs) override;
    void stopContinuous(JZSensor *sensor) override;

    // 传感器 id, 用于 JZSensor::findInfoById / setInfo
    static const char *kId;   // "esp32_die_temp"

private:
    static void timerCallback(void *arg);

    bool                  m_open = false;
    void                 *m_timer = nullptr;   // esp_timer_handle_t, 仅 ESP 平台使用
    void                 *m_handle = nullptr;  // temperature_sensor_handle_t
    JZSensor             *m_contSensor = nullptr;
};

#endif // !JZ_SENSOR_DRIVER_ESP32_H_
