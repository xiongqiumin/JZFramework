#include "JZCameraDriverEsp32.h"

#include <cstring>

#include "esp_camera.h"
#include "sensor.h"

JZCameraDriverEsp32::~JZCameraDriverEsp32()
{
    if (m_init)
        deinit();
}

bool JZCameraDriverEsp32::init(const Config &cfg)
{
    if (m_init)
        return true;

    camera_config_t cam = {};
    cam.pin_pwdn      = cfg.pinPwdn;
    cam.pin_reset     = cfg.pinReset;
    cam.pin_xclk      = cfg.pinXclk;
    cam.pin_sscb_sda  = cfg.pinSiod;
    cam.pin_sscb_scl  = cfg.pinSioc;
    cam.pin_d7 = cfg.pinY9; cam.pin_d6 = cfg.pinY8; cam.pin_d5 = cfg.pinY7;
    cam.pin_d4 = cfg.pinY6; cam.pin_d3 = cfg.pinY5; cam.pin_d2 = cfg.pinY4;
    cam.pin_d1 = cfg.pinY3; cam.pin_d0 = cfg.pinY2;
    cam.pin_vsync = cfg.pinVsync;
    cam.pin_href  = cfg.pinHref;
    cam.pin_pclk  = cfg.pinPclk;
    cam.xclk_freq_hz = cfg.xclkFreqHz;
    cam.ledc_timer   = LEDC_TIMER_0;
    cam.ledc_channel = LEDC_CHANNEL_0;
    cam.pixel_format = (cfg.format == JZCameraFrame::JPEG) ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
    cam.frame_size   = FRAMESIZE_QVGA;          // 初值, 随后由 setResolution 调整
    cam.jpeg_quality = cfg.jpegQuality;
    cam.fb_count     = cfg.fbCount;

    if (esp_camera_init(&cam) != ESP_OK)
        return false;

    m_init = true;
    setResolution(cfg.frameWidth, cfg.frameHeight);   // 应用目标分辨率
    return true;
}

void JZCameraDriverEsp32::deinit()
{
    if (m_init)
    {
        esp_camera_deinit();
        m_init = false;
    }
}

bool JZCameraDriverEsp32::setResolution(int width, int height)
{
    if (!m_init)
        return false;
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
        return false;

    int area = width * height;
    framesize_t fs = FRAMESIZE_QVGA;
    if (area <= 160 * 120)       fs = FRAMESIZE_QQVGA;
    else if (area <= 320 * 240)  fs = FRAMESIZE_QVGA;
    else if (area <= 640 * 480)  fs = FRAMESIZE_VGA;
    else if (area <= 800 * 600)  fs = FRAMESIZE_SVGA;
    else if (area <= 1024 * 768) fs = FRAMESIZE_XGA;
    else if (area <= 1280 * 720) fs = FRAMESIZE_HD;
    else                         fs = FRAMESIZE_UXGA;

    return s->set_framesize(s, fs) == 0;
}

std::vector<JZCameraDriverEsp32::Resolution> JZCameraDriverEsp32::supportedResolutions()
{
    return {
        {160, 120}, {320, 240}, {640, 480},
        {800, 600}, {1024, 768}, {1280, 720}, {1600, 1200}
    };
}

bool JZCameraDriverEsp32::capture(JZCameraFrame &out)
{
    if (!m_init)
        return false;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
        return false;

    JZCameraFrame::Format fmt = (fb->format == PIXFORMAT_JPEG) ? JZCameraFrame::JPEG : JZCameraFrame::RGB565;
    out.alloc(fb->width, fb->height, fmt);

    size_t copy = (size_t)out.dataSize() < fb->len ? (size_t)out.dataSize() : fb->len;
    memcpy(out.data(), fb->buf, copy);

    esp_camera_fb_return(fb);
    return true;
}
