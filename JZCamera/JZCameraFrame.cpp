#include "JZCameraFrame.h"

JZCameraFrame::JZCameraFrame() = default;
JZCameraFrame::~JZCameraFrame() = default;

void JZCameraFrame::alloc(int width, int height, Format format)
{
    m_width = width;
    m_height = height;
    m_format = format;

    int bpp = bytesPerPixel();
    int size = (format == JPEG) ? (width * height) : (width * height * bpp);
    m_buffer.alloc(size);
}

int JZCameraFrame::bytesPerPixel() const
{
    switch (m_format)
    {
    case RGB565: return 2;
    case YUV422: return 2;
    case RGB888: return 3;
    case GRAY8:  return 1;
    case JPEG:   return 0;
    }
    return 0;
}
