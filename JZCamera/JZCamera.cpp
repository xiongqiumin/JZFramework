#include "JZCamera.h"

JZCamera::JZCamera(JZObject *parent)
    : JZObject(parent)
    , sigFrameReady(this)
    , sigError(this)
{
    m_timer = new JZTimer(this);
    connect(m_timer, &JZTimer::sigTimeout, this, &JZCamera::onTimeout);
}

JZCamera::~JZCamera()
{
    close();
}

bool JZCamera::open()
{
    return open(m_config);
}

bool JZCamera::open(const JZCameraDriver::Config &cfg)
{
    if (m_open)
        return true;

    JZCameraDriver *drv = JZCameraDriver::instance();
    if (!drv)
    {
        sigError.emit("JZCamera: no camera driver installed");
        return false;
    }

    m_config = cfg;
    if (!drv->init(cfg))
    {
        sigError.emit("JZCamera: driver init failed");
        return false;
    }

    m_open = true;
    return true;
}

void JZCamera::close()
{
    stop();
    if (m_open)
    {
        if (JZCameraDriver *drv = JZCameraDriver::instance())
            drv->deinit();
        m_open = false;
    }
}

bool JZCamera::isOpen() const
{
    return m_open;
}

bool JZCamera::capture(JZCameraFrame &frame)
{
    if (!m_open)
        return false;
    JZCameraDriver *drv = JZCameraDriver::instance();
    if (!drv)
        return false;
    return drv->capture(frame);
}

bool JZCamera::setResolution(int width, int height)
{
    if (!m_open)
        return false;
    JZCameraDriver *drv = JZCameraDriver::instance();
    if (!drv)
        return false;
    return drv->setResolution(width, height);
}

std::vector<JZCameraDriver::Resolution> JZCamera::supportedResolutions() const
{
    if (JZCameraDriver *drv = JZCameraDriver::instance())
        return drv->supportedResolutions();
    return {};
}

bool JZCamera::start(int fps)
{
    if (!m_open)
        return false;
    if (m_streaming)
        return true;

    m_fps = (fps > 0) ? fps : 10;
    m_timer->start(1000 / m_fps);
    m_streaming = true;
    return true;
}

void JZCamera::stop()
{
    if (m_timer)
        m_timer->stop();
    m_streaming = false;
}

bool JZCamera::isStreaming() const
{
    return m_streaming;
}

int JZCamera::fps() const
{
    return m_fps;
}

void JZCamera::onTimeout()
{
    JZCameraFrame frame;
    if (capture(frame))
        sigFrameReady.emit(frame);
    else
        sigError.emit("JZCamera: capture failed");
}
