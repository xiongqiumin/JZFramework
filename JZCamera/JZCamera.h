#ifndef JZ_CAMERA_H_
#define JZ_CAMERA_H_
#include <vector>
#include "JZObject.h"
#include "JZTimer.h"
#include "JZCameraFrame.h"
#include "JZCameraDriver.h"

// 面向用户的摄像头类(平台无关)。底层操作全部委派给 JZCameraDriver 单例,
// 因此 JZCamera 自身不含任何硬件相关代码, 可在 ESP32 / 模拟器 / PC 上通用。
//
// 典型用法:
//   auto *cam = new JZCamera(this);
//   cam->open();                       // 使用默认 ESP32-CAM 配置
//   connect(cam, &JZCamera::sigFrameReady, [](const JZCameraFrame &f){
//       // 处理 f.data()/f.width()/f.height()
//   });
//   cam->start(15);                    // 以 15fps 连续采集
class JZFRAMEWORK_API JZCamera : public JZObject
{
public:
    explicit JZCamera(JZObject *parent = nullptr);
    ~JZCamera() override;

    // 使用默认(或上次 open 传入的)配置打开摄像头
    bool open();
    bool open(const JZCameraDriver::Config &cfg);
    void close();
    bool isOpen() const;

    // 同步抓取一帧(成功后 frame 内含有效像素)
    bool capture(JZCameraFrame &frame);

    // 设置分辨率(需 open 后调用)
    bool setResolution(int width, int height);
    std::vector<JZCameraDriver::Resolution> supportedResolutions() const;

    // 连续采集: 每隔 1000/fps 毫秒抓取一帧并发出 sigFrameReady
    bool start(int fps = 10);
    void stop();
    bool isStreaming() const;
    int  fps() const;

jz_signals:
    // 每帧就绪(单拍 capture 或流模式 start 都会触发)
    JZSignal<JZCameraFrame> sigFrameReady;
    // 错误信息
    JZSignal<const std::string &> sigError;

protected:
    JZ_DISABLE_COPY(JZCamera)

    void onTimeout();

    bool    m_open = false;
    bool    m_streaming = false;
    int     m_fps = 10;
    JZTimer *m_timer = nullptr;
    JZCameraDriver::Config m_config;
};

#endif // !JZ_CAMERA_H_
