#ifndef JZ_CAMERA_FRAME_H_
#define JZ_CAMERA_FRAME_H_

#include "JZSharedBuffer.h"

// 一帧图像数据(与平台无关)。像素数据存于 JZSharedBuffer(引用计数, 拷贝成本低)。
class JZCameraFrame
{
public:
    // 像素格式
    enum Format {
        RGB565 = 0,
        RGB888,
        GRAY8,
        JPEG,
        YUV422
    };

    JZCameraFrame();
    ~JZCameraFrame();

    // 分配指定尺寸/格式的缓冲区
    void alloc(int width, int height, Format format);

    int    width()  const { return m_width; }
    int    height() const { return m_height; }
    Format format() const { return m_format; }
    int    dataSize() const { return m_buffer.size(); }

    char       *data()       { return m_buffer.data(); }
    const char *data() const { return m_buffer.data(); }

    // 每像素字节数(压缩格式 JPEG 返回 0)
    int bytesPerPixel() const;

protected:
    int           m_width = 0;
    int           m_height = 0;
    Format        m_format = RGB565;
    JZSharedBuffer m_buffer;
};

#endif // !JZ_CAMERA_FRAME_H_
