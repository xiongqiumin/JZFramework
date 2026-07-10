#ifndef JZ_CAMERA_DRIVER_H_
#define JZ_CAMERA_DRIVER_H_

#include <vector>
#include "JZCameraFrame.h"

// 摄像头驱动抽象基类(单例)。各平台(ESP32-CAM / 模拟器)实现自己的子类,
// 并在程序初始化时通过 setInstance() 安装。JZCamera 的所有底层操作都委派给它。
class JZCameraDriver
{
public:
    // 支持的分辨率项
    struct Resolution { int width; int height; };

    // 摄像头硬件配置(引脚等)。默认面向 ESP32-CAM(AI-Thinker) 板载 OV2640。
    struct Config {
        int pinPwdn  = 32;
        int pinReset = -1;
        int pinXclk  = 0;
        int pinSiod  = 26;   // SCCB SDA
        int pinSioc  = 27;   // SCCB SCL
        int pinY9 = 35, pinY8 = 34, pinY7 = 39, pinY6 = 36;
        int pinY5 = 21, pinY4 = 19, pinY3 = 18, pinY2 = 5;
        int pinVsync = 25, pinHref = 23, pinPclk = 22;
        int xclkFreqHz  = 20000000;
        int frameWidth  = 320;
        int frameHeight = 240;
        JZCameraFrame::Format format = JZCameraFrame::RGB565;
        int jpegQuality = 12;
        int fbCount = 2;
    };

    JZCameraDriver() = default;
    virtual ~JZCameraDriver() = default;

    static JZCameraDriver *instance();
    static void setInstance(JZCameraDriver *inst);
    static void clearInstance();

    virtual bool init(const Config &cfg) = 0;
    virtual void deinit() = 0;
    virtual bool isInit() const = 0;

    virtual bool setResolution(int width, int height) = 0;
    virtual std::vector<Resolution> supportedResolutions() = 0;

    // 抓取一帧到 out(由驱动分配/填充像素数据)。返回 false 表示抓取失败。
    virtual bool capture(JZCameraFrame &out) = 0;

protected:
    JZ_DISABLE_COPY(JZCameraDriver)

    static JZCameraDriver *s_instance;
};

#endif // !JZ_CAMERA_DRIVER_H_
