#ifndef JZ_CAMERA_DRIVER_ESP32_H_
#define JZ_CAMERA_DRIVER_ESP32_H_

#include "JZCameraDriver.h"

// 基于 ESP-IDF esp_camera 的 JZCameraDriver 实现(ESP32-CAM / OV2640 等)。
// 注意: 本文件包含 esp_camera.h, 仅在 ESP-IDF 环境下参与编译(见 JZCamera.cmake)。
class JZCameraDriverEsp32 : public JZCameraDriver
{
public:
    JZCameraDriverEsp32() = default;
    ~JZCameraDriverEsp32() override;

    bool init(const Config &cfg) override;
    void deinit() override;
    bool isInit() const override { return m_init; }

    bool setResolution(int width, int height) override;
    std::vector<Resolution> supportedResolutions() override;

    bool capture(JZCameraFrame &out) override;

private:
    bool m_init = false;
};

#endif // !JZ_CAMERA_DRIVER_ESP32_H_
