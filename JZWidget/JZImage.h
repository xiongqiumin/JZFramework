#ifndef JZ_IMAGE_H_
#define JZ_IMAGE_H_
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "JZByteArray.h"
#include "JZColor.h"

// 图像数据: RGBA8888 像素缓冲, 渲染后端 (JZGdiBackend) 懒缓存为位图。
class JZFRAMEWORK_API JZImageData
{
public:
    int width() const { return m_w; }
    int height() const { return m_h; }
    const uint8_t* pixels() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    uint8_t* pixels() { return m_pixels.empty() ? nullptr : m_pixels.data(); }

    void setPixels(int w, int h, const uint8_t* rgba)
    {
        m_w = w;
        m_h = h;
        if (w > 0 && h > 0 && rgba)
            m_pixels.assign(rgba, rgba + (size_t)w * h * 4);
        else {
            m_w = 0;
            m_h = 0;
            m_pixels.clear();
        }
    }

private:
    int m_w = 0;
    int m_h = 0;
    std::vector<uint8_t> m_pixels;
};

class JZFRAMEWORK_API JZImage
{
public:
    // 图像/文件流格式。完整集合与 JZImageReader::Format 一致(解码探测用),
    // 其中 encode/save 实际可写的子集为 Png/Bmp/Tga/Jpeg (同 JZImageWriter)。
    enum class Format
    {
        Unknown,
        Png,
        Jpeg,
        Bmp,
        Gif,
        WebP,
        Tga,
        Psd
    };

    JZImage() = default;
    // 从文件立即解码 (Qt 风格); 格式按文件头自动探测, 失败得到 isNull()==true
    // 的空对象, 不保存文件路径。
    explicit JZImage(const std::string& file);
    // 从内存数据立即解码; data 在调用期间保持有效即可, 内部会拷贝像素。
    JZImage(const char* data, int len);

    bool isNull() const;   // 无像素数据
    std::shared_ptr<JZImageData> data() const;

    int width() const;
    int height() const;

    // 新建 w×h 纯色画布 (默认透明), 供 JZPainter 离屏绘制:
    //   JZImage img = JZImage::create(200, 100);
    //   { JZPainter p(img); p.fillRect(...); }  // 析构时绘入 img
    static JZImage create(int w, int h, const JZColor& color = JZColor(0, 0, 0, 0));

    // ---- 编解码 ----

    // 解码压缩图像数据 (PNG/JPEG/BMP/... ) 为新 JZImage, 格式自动探测;
    // 数据损坏/格式不支持时返回 isNull()==true 的空对象。
    static JZImage decode(const JZByteArray& data);

    // 用已有的 RGBA8888 像素构造 ("非解码"路径: 截屏/运行时生成图像)。
    // 内部拷贝一份像素; w/h <= 0 或 rgba 为空时返回 isNull()==true 的空对象。
    static JZImage fromRGBA(const uint8_t* rgba, int w, int h);

    // 用已有的 JZImageData 构造: **不拷贝像素**, 与原图共享同一份数据
    // (用于把后端/画布手里的像素数据重新包成 JZImage 去调 encode/scale/pixmap 等
    //  以 JZImage 为入口的接口)。空指针得到空图。
    static JZImage fromData(const std::shared_ptr<JZImageData>& data);

    // 编码当前像素为压缩图像字节流; 仅 Png/Bmp/Tga/Jpeg 可写,
    // 其它格式或图像无像素时返回空 JZByteArray。JPEG 质量固定 90。
    JZByteArray encode(Format format) const;

    // 保存为文件, 编码格式按扩展名推断 (.png/.bmp/.tga/.jpg/.jpeg);
    // 扩展名不支持 / 图像无像素 / 写盘失败均返回 false。
    bool save(const std::string& file) const;

    // ---- 像素缩放 (RGBA8888, stb_image_resize2) ----

    // 目标尺寸模式 (名称/语义镜像 Qt::AspectRatioMode):
    //   IgnoreAspectRatio          直接拉伸到 w×h (默认)
    //   KeepAspectRatio            保持宽高比缩放到 w×h 内 (一边可能 < 目标)
    //   KeepAspectRatioByExpanding 保持宽高比缩放到覆盖 w×h (一边可能 > 目标)
    enum class AspectRatioMode { IgnoreAspectRatio, KeepAspectRatio, KeepAspectRatioByExpanding };

    // 平滑档位 (镜像 Qt::TransformationMode):
    //   FastTransformation   点采样 (等价最近邻), Qt 默认档
    //   SmoothTransformation 双线性
    enum class TransformationMode { FastTransformation, SmoothTransformation };

    // 返回新图的缩放 (参数序/语义同 Qt QImage::scaled, 不改本对象)。
    // 仅在 w/h > 0 且本图有像素时成功; 失败返回 isNull()==true 的空图。
    JZImage scaled(int w, int h,
                   AspectRatioMode aspectRatioMode = AspectRatioMode::IgnoreAspectRatio,
                   TransformationMode transformMode = TransformationMode::FastTransformation) const;

private:
    std::shared_ptr<JZImageData> m_data;
};

#endif
